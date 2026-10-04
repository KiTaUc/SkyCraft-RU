#pragma once

#include "Link.h"

namespace skycraft
{
	// Shared runtime state between the per-frame update, input sink and renderer hook.
	struct Runtime
	{
		// Minecraft is connected, in its world, has acknowledged our last teleport, and Skyrim
		// isn't loading: MC's player position drives Skyrim's player.
		std::atomic<bool> puppeting{ false };
		// The same, or waiting for Minecraft to arrive after a teleport: Skyrim's own controls
		// don't move its player either way (it would wander off from where Minecraft is going).
		std::atomic<bool> minecraftOwnsPlayer{ false };
		// A Minecraft GUI screen (inventory, chat, ...) is open: mouse moves MC's cursor.
		std::atomic<bool> mcScreenOpen{ false };
		// A Skyrim menu (journal, dialogue, console, loading, ...) owns input.
		std::atomic<bool> skyrimMenuOpen{ false };
		// MC reports its player is in a world (overlay should be drawn).
		std::atomic<bool> mcInWorld{ false };

		// Look direction in MC degrees; integrated from raw mouse input (main thread).
		float yaw{ 0.0f };
		float pitch{ 0.0f };
		bool  lookInitialized{ false };
		float sensitivity{ 0.5f };

		// Virtual MC cursor (overlay pixels) while an MC screen is open.
		std::atomic<int> cursorX{ 0 };
		std::atomic<int> cursorY{ 0 };
		std::atomic<int> viewportW{ 1920 };
		std::atomic<int> viewportH{ 1080 };

		// The player's feet as the camera sees them this frame (Minecraft coordinates, Skyrim's
		// own interpolation of Minecraft's ticks). The third-person body is drawn here. Main thread.
		double feetX{ 0.0 }, feetY{ 0.0 }, feetZ{ 0.0 };
		bool   feetValid{ false };

		// Minecraft's crosshair is on screen (first person, no Minecraft screen open) at this GUI
		// scale: the overlay draws it inverted against Skyrim's picture, as Minecraft does.
		std::atomic<bool> mcCrosshair{ false };
		std::atomic<int>  mcGuiScale{ 0 };
	};

	Runtime& State();

	namespace Game
	{
		void Install();
		void OnGameLoaded();
		// A Skyrim menu that takes the mouse or pauses the game is open (checked live: while it
		// pauses the game, the per-frame update that normally tracks it doesn't run).
		bool SkyrimMenuOpen();
		// Called from Present: logs rendered camera vs Minecraft eye when a check is due.
		void CheckRenderedCamera();
		// Third-person diagnostics, from where the blocks are drawn: the camera Skyrim renders this
		// frame, and the order of the frame's steps ('P' player update, 'H' camera update hook,
		// 'D' block drawing, 'X' Present).
		void NoteRenderedCamera(const RE::NiPoint3& a_pos, const RE::NiMatrix3& a_rot);
		void NoteFrameStep(char a_step);
	}

	// SkyCraft.ini [Debug] bDiagnostics: the detailed per-frame timing, camera, combat and lighting logs
	// used while developing (several lines a second). Off by default.
	bool DiagnosticsEnabled();

	namespace CrashLog
	{
		// Logs where Skyrim crashed (and writes a minidump) if it does.
		void Install();
	}

	namespace Launcher
	{
		// At plugin load: starts Minecraft (per SkyCraft.ini) unless it's already running.
		void StartMinecraft();

		enum class Status
		{
			kOff,         // not started by SkyCraft (bStartWithSkyrim = 0)
			kRunning,     // was already running
			kStarting,    // being started
			kSignIn,      // first start of the bundled Minecraft: Prism asks for a Microsoft account
			kNoLauncher,  // nothing to start it with
			kFailed,      // starting it failed
		};
		Status GetStatus();

		// A Minecraft with the SkyCraft mod is running (it holds SkyCraft's mutex from early on).
		bool MinecraftRunning();
		// Prism Launcher is running (downloading, waiting on a sign-in or showing an error).
		bool PrismRunning();
	}

	namespace Input
	{
		void Install();
		// Mouse deltas accumulated since the last frame (main thread).
		void ConsumeLook(float& a_dx, float& a_dy);
		// Tells MC to release everything (input focus moved to Skyrim).
		void ReleaseAll();
	}

	namespace Input
	{
		// While Minecraft drives the player, Skyrim's Activate prompt shows G (our activate key).
		void SetActivatePromptKey(bool a_minecraftControls);
	}

	namespace Combat
	{
		void Install();
		// Main thread, once per frame: actor table out, Minecraft's hits in, player damage bridged.
		void PerFrame(RE::PlayerCharacter* a_player, bool a_puppeting, float a_delta);
		// Someone nearby is fighting the player (in combat with them as its target). Main thread.
		bool PlayerEngaged();
	}

	namespace BlockLights
	{
		// Render messages (main thread): a section's light-emitting blocks; a world change.
		void OnLights(const std::uint8_t* a_data, std::uint32_t a_bytes);
		void Clear();
		// Main thread, every frame: Skyrim point lights on the brightest emitters near the player
		// (Minecraft coords; null turns them all off).
		void Update(const McVec* a_player, float a_delta);
		// One of those lights (the block shader leaves them out: Minecraft's blocks carry
		// Minecraft's own block light).
		bool IsOurs(const RE::NiLight* a_light);
		// What a Minecraft block does to whoever stands in it (proto::BlockHazard: lava, fire, magma).
		std::uint8_t HazardAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z);
	}

	namespace PathAvoid
	{
		// Hooks Skyrim's NPC path setup so paths route around Minecraft's solid blocks.
		void Install();
		// The goal of the NPC's latest path (Skyrim position), if one was seen. Any thread.
		bool GoalOf(RE::FormID a_actor, RE::NiPoint3& a_out);
	}

	namespace NpcBlocks
	{
		// Render messages (main thread): a section's solid Minecraft blocks; a world change.
		void OnSolids(const std::uint8_t* a_data, std::uint32_t a_bytes);
		void Clear();
		bool SolidAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z);
		// For the blocks' contact shadows (any thread): sets a_bit in a_out[x + w*(y + h*z)] for
		// each solid block of the box at a_origin (Minecraft block coords) of a_size blocks. The
		// generation changes whenever any section's blocks do.
		void CopySolids(const std::int32_t a_origin[3], const std::int32_t a_size[3], std::uint32_t* a_out, std::uint32_t a_bit);
		std::uint32_t Generation();
		// Main thread, every frame: NPCs overlapping solid Minecraft blocks are pushed back out.
		void PushActorsOut(RE::PlayerCharacter* a_player, float a_delta);
	}

	namespace WorldRender
	{
		// Hooks Skyrim's frame so blocks are drawn into its scene before post-processing.
		void Install();
		// A Minecraft arrow stuck in a Skyrim actor where it hit (MC coords, flight yaw/pitch):
		// pinned to the nearest bone and drawn from then on. Main thread.
		void StickArrow(RE::FormID a_actor, float a_x, float a_y, float a_z, float a_yawDeg, float a_pitchDeg);
		// Main thread, every frame: once the Skyrim player is dead, their Minecraft body replaces
		// Skyrim's on its ragdoll (a_minecraftBody: Minecraft is connected and in its world).
		void UpdateRagdoll(RE::PlayerCharacter* a_player, bool a_minecraftBody);
		// Present hook, before the overlay: drains Minecraft's meshes; draws the blocks, arrows,
		// items and the outline here only if they couldn't be drawn inside Skyrim's frame.
		void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain);
		// Debug: saves the frame as a PNG when <SKSE log dir>/skycraft_capture.request exists.
		void CaptureIfRequested(ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain);
	}

	namespace Overlay
	{
		void Install();
	}
}
