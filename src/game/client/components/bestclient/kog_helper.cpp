/* Copyright © 2026 KOG Helper contributors (BestClient fork) */
#include "kog_helper.h"

#include <base/color.h>
#include <base/dbg.h>
#include <base/math.h>
#include <base/time.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/components/camera.h>
#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

#include <algorithm>
#include <array>
#include <cstdlib>

namespace
{
constexpr float TILE_SIZE = 32.0f;

int ClampInt(int Value, int Min, int Max)
{
	return std::min(Max, std::max(Min, Value));
}
} // namespace

void CKogHelper::OnReset()
{
	m_TickCounter = 0;
	m_OverrideDirection = 0;
	m_OverrideJump = 0;
	m_OverrideFire = 0;
	m_OverrideTicks = 0;
	m_CooldownTicks = 0;
	m_LastMarkerWarnTime = 0;
	m_PreviewLen = 0;
	m_PreviewFreezeTick = -1;
}

bool CKogHelper::CanIntervene() const
{
	// The user opted in explicitly with bc_kog_input. Server-side trust checks
	// are intentionally gone: correction follows the player wherever they play.
	return g_Config.m_BcKogInput != 0 && Client()->State() == IClient::STATE_ONLINE;
}

bool CKogHelper::IsInputAllowed() const
{
	return CanIntervene();
}

void CKogHelper::Predict(int Direction, int Jump, int Fire, int Horizon, SPreviewPoint *pOut, int *pOutLen, int *pOutFreezeTick) const
{
	*pOutLen = 0;
	*pOutFreezeTick = -1;
	if(!GameClient()->Collision())
		return;

	CCharacterCore Core = GameClient()->m_PredictedChar;
	Core.SetCoreWorld(&GameClient()->m_PredictedWorld.m_Core, GameClient()->Collision(), GameClient()->m_PredictedWorld.Teams());
	Core.m_Input = GameClient()->m_PredictedChar.m_Input;
	Core.m_Input.m_Direction = Direction;
	Core.m_Input.m_Jump = Jump != 0 ? 1 : 0;
	Core.m_Input.m_Fire = Fire != 0 ? 1 : 0;
	Core.m_Direction = Direction;

	// This is only a movement preview. Never simulate the player's hook here:
	// stale hook targets/team data used to crash the client while hooking.
	// Drop the copied hook target through the public setter so attached-player
	// bookkeeping stays consistent, and clear every remaining hook field.
	Core.SetHookedPlayer(-1);
	Core.m_AttachedPlayers.clear();
	Core.m_Input.m_Hook = 0;
	Core.m_HookState = HOOK_IDLE;
	Core.m_HookTick = 0;
	Core.m_HookPos = Core.m_Pos;
	Core.m_HookDir = vec2(0.0f, 0.0f);
	Core.m_HookTeleBase = vec2(0.0f, 0.0f);

	const int Ticks = ClampInt(Horizon, 1, MAX_PREVIEW_TICKS);
	for(int Tick = 0; Tick < Ticks; ++Tick)
	{
		Core.Tick(true, false);

		const int TileX = round_to_int(Core.m_Pos.x / TILE_SIZE);
		const int TileY = round_to_int(Core.m_Pos.y / TILE_SIZE);
		const int Tile = GameClient()->Collision()->GetTile(TileX, TileY);
		const int FrontTile = GameClient()->Collision()->GetFrontTile(TileX, TileY);
		const bool Freeze = Tile == TILE_FREEZE || FrontTile == TILE_FREEZE;
		const bool Unfreeze = Tile == TILE_UNFREEZE || FrontTile == TILE_UNFREEZE;

		pOut[Tick].m_Pos = Core.m_Pos;
		pOut[Tick].m_Freeze = Freeze && !Unfreeze;
		*pOutLen = Tick + 1;

		if(pOut[Tick].m_Freeze)
		{
			*pOutFreezeTick = Tick;
			break;
		}
	}
}

void CKogHelper::LogIntervention(int FromDirection, int ToDirection, int Jump, int Fire, int DesiredFreezeTick, int BestFreezeTick)
{
	// Every correction is visible in the console: no silent input rewriting.
	if(time_get() - m_LastLogTime < time_freq() / 2)
		return;
	m_LastLogTime = time_get();

	dbg_msg("kog-avoid", "server_check=off preset=%d tick=%d direction=%d->%d jump=%d hammer=%d freeze_tick=%d->%d",
		g_Config.m_BcKogPreset, m_TickCounter, FromDirection, ToDirection, Jump, Fire, DesiredFreezeTick, BestFreezeTick);
}

void CKogHelper::LogMarkerOnly(int FreezeTick)
{
	if(time_get() - m_LastMarkerWarnTime < time_freq())
		return;
	m_LastMarkerWarnTime = time_get();

	dbg_msg("kog-avoid", "marker_only=1 freeze_tick=%d reason=input_correction_disabled", FreezeTick);
}

void CKogHelper::ApplyAvoid(int *pDirection, int *pJump, int *pFire)
{
	++m_TickCounter;

	const int DesiredDirection = ClampInt(*pDirection, -1, 1);
	const int DesiredJump = *pJump != 0 ? 1 : 0;
	const int DesiredFire = *pFire != 0 ? 1 : 0;

	const int Preset = ClampInt(g_Config.m_BcKogPreset, 0, 2);
	int Horizon = ClampInt(g_Config.m_BcKogHorizon, 10, MAX_PREVIEW_TICKS);
	if(Preset == 2)
		Horizon = std::max(Horizon, 32);

	// The visual preview is always calculated. Input correction is not.
	int AppliedDirection = DesiredDirection;
	int AppliedJump = DesiredJump;
	int AppliedFire = DesiredFire;

	if(m_CooldownTicks > 0)
		--m_CooldownTicks;

	if(CanIntervene())
	{
		if(m_OverrideTicks > 0)
		{
			AppliedDirection = m_OverrideDirection;
			AppliedJump = m_OverrideJump;
			AppliedFire = m_OverrideFire;
			--m_OverrideTicks;
		}
		else
		{
			struct SCandidate
			{
				int m_Direction;
				int m_Jump;
				int m_Fire;
				int m_FreezeTick;
				float m_Score;
			};

			std::array<SCandidate, 12> aCandidates = {};
			int NumCandidates = 0;
			float DesiredScore = 0.0f;
			int DesiredFreezeTick = -1;
			int BestIndex = -1;

			// Later freeze is better; no freeze at all is best. A path that
			// delays freeze by even one tick counts as an improvement in Rage.
			const auto FreezeQuality = [Horizon](int FreezeTick) {
				return FreezeTick < 0 ? Horizon + 8 : FreezeTick;
			};

			const int aDirections[] = {-1, 0, 1};
			for(int Direction : aDirections)
			{
				for(int Jump = 0; Jump < 2; ++Jump)
				{
					for(int Fire = 0; Fire < 2; ++Fire)
					{
						int FreezeTick = -1;
						int PathLen = 0;
						Predict(Direction, Jump, Fire, Horizon, m_aPreview.data(), &PathLen, &FreezeTick);

						float Score = static_cast<float>(FreezeQuality(FreezeTick)) * 12.0f;
						Score -= static_cast<float>(std::abs(Direction - DesiredDirection)) * (Preset == 2 ? 1.0f : 4.0f);
						Score -= Jump * (Preset == 2 ? 1.0f : 3.0f);
						Score -= Fire * (Preset == 2 ? 0.5f : 2.0f);
						if(Direction != 0 && GameClient()->m_PredictedChar.m_Vel.x * Direction < 0.0f)
							Score -= 2.0f;

						aCandidates[NumCandidates] = {Direction, Jump, Fire, FreezeTick, Score};
						if(Direction == DesiredDirection && Jump == DesiredJump && Fire == DesiredFire)
						{
							DesiredScore = Score;
							DesiredFreezeTick = FreezeTick;
						}
						if(BestIndex < 0 || Score > aCandidates[BestIndex].m_Score)
							BestIndex = NumCandidates;
						++NumCandidates;
					}
				}
			}

			const int Threshold = ClampInt(g_Config.m_BcKogSafety, 0, 10);
			const int MinGain = Preset == 2 ? 1 : (Preset == 1 ? 4 : 2);
			const SCandidate &Best = aCandidates[BestIndex];
			const int DesiredQuality = FreezeQuality(DesiredFreezeTick);
			const int BestQuality = FreezeQuality(Best.m_FreezeTick);
			const bool BestIsSafer = BestQuality >= DesiredQuality + MinGain;
			const bool WorthIt = Best.m_Score > DesiredScore + (Preset == 2 ? 0 : Threshold);

			if(DesiredFreezeTick >= 0 && BestIsSafer && WorthIt && m_CooldownTicks == 0)
			{
				AppliedDirection = Best.m_Direction;
				AppliedJump = Best.m_Jump;
				AppliedFire = Best.m_Fire;

				m_OverrideDirection = AppliedDirection;
				m_OverrideJump = AppliedJump;
				m_OverrideFire = AppliedFire;
				m_OverrideTicks = Preset == 1 ? 4 : (Preset == 2 ? 14 : 6);
				m_CooldownTicks = Preset == 2 ? 0 : std::max(Preset == 1 ? 4 : 1, ClampInt(g_Config.m_BcKogCooldown, 0, 20));

				LogIntervention(DesiredDirection, AppliedDirection, AppliedJump, AppliedFire, DesiredFreezeTick, Best.m_FreezeTick);
			}
		}
	}
	else
	{
		m_OverrideTicks = 0;
	}

	*pDirection = AppliedDirection;
	*pJump = AppliedJump;
	*pFire = AppliedFire;

	m_PreviewDirection = AppliedDirection;
	Predict(AppliedDirection, AppliedJump, AppliedFire, Horizon, m_aPreview.data(), &m_PreviewLen, &m_PreviewFreezeTick);
	if(m_PreviewFreezeTick >= 0 && !CanIntervene())
		LogMarkerOnly(m_PreviewFreezeTick);
}

void CKogHelper::OnRender()
{
	if(g_Config.m_BcKogMarker == 0 || Client()->State() != IClient::STATE_ONLINE)
		return;
	if(m_PreviewLen < 2)
		return;

	const CScreenRect PrevScreen = Graphics()->GetScreen();
	float Width = 0.0f;
	float Height = 0.0f;
	Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom, &Width, &Height);
	const vec2 Center = GameClient()->m_Camera.m_Center;
	Graphics()->MapScreen(CScreenRect(Center.x - Width * 0.5f, Center.y - Height * 0.5f, Width, Height));

	Graphics()->TextureClear();
	Graphics()->LinesBegin();

	// Safe part of the trajectory: cyan. The part that ends in freeze: red.
	Graphics()->SetColor(ColorRGBA(0.15f, 0.85f, 1.0f, 0.65f));
	for(int i = 0; i + 1 < m_PreviewLen; ++i)
	{
		if(m_aPreview[i].m_Freeze || m_aPreview[i + 1].m_Freeze)
			continue;
		const IGraphics::CLineItem Line(m_aPreview[i].m_Pos, m_aPreview[i + 1].m_Pos);
		Graphics()->LinesDraw(&Line, 1);
	}

	Graphics()->SetColor(ColorRGBA(1.0f, 0.12f, 0.12f, 0.95f));
	for(int i = 0; i + 1 < m_PreviewLen; ++i)
	{
		if(!m_aPreview[i].m_Freeze && !m_aPreview[i + 1].m_Freeze)
			continue;
		const IGraphics::CLineItem Line(m_aPreview[i].m_Pos, m_aPreview[i + 1].m_Pos);
		Graphics()->LinesDraw(&Line, 1);
		// Second, slightly shifted line makes the warning segment readable.
		const IGraphics::CLineItem Glow(m_aPreview[i].m_Pos.x, m_aPreview[i].m_Pos.y + 1.0f, m_aPreview[i + 1].m_Pos.x, m_aPreview[i + 1].m_Pos.y + 1.0f);
		Graphics()->LinesDraw(&Glow, 1);
	}

	Graphics()->LinesEnd();
	Graphics()->MapScreen(PrevScreen);
}
