#include "Game.h"

#include "Collision.h"
#include "Dig.h"
#include "Perf.h"

namespace skycraft
{
	Runtime& State()
	{
		static Runtime runtime;
		return runtime;
	}

	namespace
	{
		constexpr float kRadToDeg = 57.2957795f;
		constexpr float kDegToRad = 0.0174532925f;
		constexpr float kSkyrimTeleportThreshold = 300.0f;  // units; bigger jumps are Skyrim moving the player

		// Everything below is only touched on the main thread (PlayerCharacter::Update).
		proto::McState mc{};
		bool           mcWasAlive = false;
		// Starts somewhere new each Skyrim run, so a Minecraft still acknowledging the last run's
		// teleport can't be taken for having arrived at this run's.
		std::uint32_t  teleportSeq = [] {
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return static_cast<std::uint32_t>(t.QuadPart) | 1u;
		}();
		float          holdMismatch = 0.0f;  // seconds Minecraft has been waiting away from Skyrim's player
		bool           teleportPending = true;
		std::uint32_t  worldId = 0;
		std::uint32_t  epoch = 0;
		RE::NiPoint3   lastSetPos{};
		bool           haveLastSet = false;
		float          hudTimer = 0.0f;
		float          savedGravity = -1.0f;  // Skyrim's controller gravity while Minecraft drives
		// Minecraft's physics ticks, on our clock. Minecraft ticks exactly every tickMs, but stamps a
		// tick only after that frame's network work and the tick itself (a few ms; more while terrain
		// streams in), and we see it on our next frame. So the tick times are locked to that exact
		// rhythm (the stamps' noise filtered out), and we render just far enough in the past that the
		// next tick has always arrived, measured from how late ticks really turn up: too little and
		// the player stops for a frame and then jumps; that happens whenever our frames get slow.
		struct Tick
		{
			proto::McState s;
			std::int64_t   at;     // when it happened (QPC), on the locked rhythm
			int            slots;  // ticks since the one we saw before (2+: we missed one)
		};
		std::deque<Tick>       tickHistory;
		std::int64_t           lastFrameQpc = 0;
		int                    stampOutliers = 0;
		double                 renderDelayMs = 10.0;  // how far in the past we render
		std::array<double, 40> tickDue{};             // last 2 s: how long each tick was already due when we first saw it, ms
		std::size_t            tickDueNext = 0;
		bool                   tickDueInit = false;
		// The third-person zoom we use: Minecraft's, eased back out after it pulls in.
		float                  zoom = 0.0f;
		std::uint32_t          zoomMode = 0;
		// Every 10 s while Minecraft drives: how smoothly its motion reached us.
		struct MotionStats
		{
			int    ticks = 0, frames = 0, lateFrames = 0, stampSamples = 0;
			double stampErrMs = 0.0, frameMsSum = 0.0, frameMsMax = 0.0;
			float  zoomTravel = 0.0f;
		} motion;
		float          motionLogTimer = 10.0f;
		// Third person: the point the camera orbits (Minecraft's eye) and how far out it sits, and
		// how far the camera Skyrim actually renders is off that orbit (diagnostics).
		std::mutex     orbitLock;
		RE::NiPoint3   orbitPivot{};
		float          orbitUnits = 0.0f;
		bool           orbitValid = false;
		float          orbitYawSpeed = 0.0f;  // degrees per second, this frame
		struct OrbitStats
		{
			int   frames = 0, turning = 0;
			float missSum = 0.0f, missMax = 0.0f, turnMissSum = 0.0f, staleSum = 0.0f, staleMax = 0.0f;
		} orbit;
		std::string    frameOrder;
		bool           frameOrderLogged = false;
		float          captureTimer = 8.0f;  // frame-capture diagnostic countdown
		// Per-frame interpolation details for the capture.
		double         dbgT = 0.0;
		std::int64_t   dbgTickQpc = 0;
		RE::NiPoint3   dbgFeet{};
		float          dbgLift = 0.0f, dbgSide = 0.0f;
		bool           dbgJumpTrigger = false;
		float          dbgSinceJumpCapture = 99.0f;
		int            dbgClimbTicks = 0;  // consecutive ticks walking uphill in third person
		const char*    dbgTriggerReason = "jump";
		// After loads/teleports Havok bodies stream in over a few frames; exporting collision
		// before then sends empty regions, so wait for the world to settle first.
		float          settleTimer = 2.0f;
		constexpr float kSettleSeconds = 1.5f;

		RE::NiPoint3      eyePos{};
		// Meshes of Skyrim's first-person model we hid for Minecraft's third-person camera.
		std::vector<RE::NiPointer<RE::BSGeometry>> hiddenFirstPerson;
		std::atomic<bool> eyeValid{ false };

		// Camera orientation straight from Minecraft's look angles (+ its bob tilt/roll), instead of
		// Skyrim's animated first-person camera bone (which sways with run/jump/land animations).
		RE::NiMatrix3 idealRot;         // what we render with
		RE::NiMatrix3 idealRotNoRoll;   // what Skyrim should produce from the angles we give it
		bool          idealValid = false;
		// Runtime discovery of Skyrim's camera-root axis convention before we take over:
		// for each column of Skyrim's matrix, which of (+/-) forward/up/right it is.
		std::array<std::array<int, 6>, 3> axisVotes{};   // [column][candidate]
		int                               axisSamples = 0;
		std::array<int, 3>                axisMap{ 0, 1, 2 };  // candidate index per column (0..5)
		bool               rotValidated = false;
		bool               rotRejected = false;
		float              devMax = 0.0f, devSum = 0.0f, dbgRotDev = 0.0f;
		int                devCount = 0;
		float              devLogTimer = 5.0f;

		RE::NiPoint3 Col(const RE::NiMatrix3& a_m, int a_c) { return { a_m.entry[0][a_c], a_m.entry[1][a_c], a_m.entry[2][a_c] }; }

		// Skyrim's first-person model (arms, quiver, weapons) stays where Skyrim's first-person
		// camera is; with Minecraft's third-person camera it would float in view (in front view,
		// right in the lens). Only its meshes are hidden, never its nodes: Skyrim's camera and
		// animation use those. Exactly the meshes hidden here are shown again afterwards.
		void HideFirstPersonMeshes(RE::PlayerCharacter* a_player, bool a_hide)
		{
			if (!a_hide) {
				for (auto& mesh : hiddenFirstPerson) {
					if (mesh && mesh->GetAppCulled()) {
						mesh->SetAppCulled(false);
					}
				}
				hiddenFirstPerson.clear();
				return;
			}
			auto* root = a_player->Get3D(true);
			if (!root) {
				return;
			}
			RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_mesh) {
				if (!a_mesh->GetAppCulled()) {
					a_mesh->SetAppCulled(true);
					hiddenFirstPerson.emplace_back(a_mesh);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		float AngleDeg(RE::NiPoint3 a_a, RE::NiPoint3 a_b)
		{
			const float la = a_a.Length(), lb = a_b.Length();
			if (la < 1e-6f || lb < 1e-6f) {
				return 180.0f;
			}
			return std::acos(std::clamp(a_a.Dot(a_b) / (la * lb), -1.0f, 1.0f)) * kRadToDeg;
		}

		// NiCamera convention: column 0 = view direction, 1 = up, 2 = right. Skyrim: X east,
		// Y north, Z up; heading clockwise from north; positive pitch looks down.
		RE::NiMatrix3 BuildCameraRotation(float a_heading, float a_pitch, float a_roll)
		{
			const float sh = std::sin(a_heading), ch = std::cos(a_heading);
			const float sp = std::sin(a_pitch), cp = std::cos(a_pitch);
			RE::NiPoint3 f{ sh * cp, ch * cp, -sp };
			RE::NiPoint3 r{ ch, -sh, 0.0f };
			RE::NiPoint3 u = r.Cross(f);
			if (a_roll != 0.0f) {
				const float cr = std::cos(a_roll), sr = std::sin(a_roll);
				const RE::NiPoint3 u2 = u * cr + r * sr;
				const RE::NiPoint3 r2 = r * cr - u * sr;
				u = u2;
				r = r2;
			}
			RE::NiMatrix3 m;
			for (int i = 0; i < 3; ++i) {
				const float fv[3] = { f.x, f.y, f.z }, uv[3] = { u.x, u.y, u.z }, rv[3] = { r.x, r.y, r.z };
				m.entry[i][0] = fv[i];
				m.entry[i][1] = uv[i];
				m.entry[i][2] = rv[i];
			}
			return m;
		}

		// Turns the camera root to the Minecraft look (once Skyrim's axis convention is known).
		void ApplyLookRotation(RE::NiAVObject* a_root)
		{
			if (!rotValidated) {
				return;
			}
			const RE::NiPoint3 fr = Col(idealRot, 0), ur = Col(idealRot, 1), rr = Col(idealRot, 2);
			const std::array<RE::NiPoint3, 6> rolled{ fr, fr * -1.0f, ur, ur * -1.0f, rr, rr * -1.0f };
			RE::NiMatrix3 m;
			for (int c = 0; c < 3; ++c) {
				const auto& v = rolled[axisMap[c]];
				m.entry[0][c] = v.x;
				m.entry[1][c] = v.y;
				m.entry[2][c] = v.z;
			}
			a_root->local.rotate = m;
			a_root->world.rotate = m;
		}

		// Puts the camera anchor and the sky at the eye (the third-person camera position).
		void PinCameraAndSky()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !camera->cameraRoot) {
				return;
			}
			auto* root = camera->cameraRoot.get();
			const bool identityParent = !root->parent || root->parent->world.translate.Length() < 0.001f;
			if (identityParent) {
				root->local.translate = eyePos;
			}
			root->world.translate = eyePos;
			camera->GetRuntimeData2().pos = eyePos;
			if (auto* sky = RE::Sky::GetSingleton(); sky && sky->root) {
				sky->root->local.translate = eyePos;
				sky->root->world.translate = eyePos;
			}
		}

		// Skyrim FOV saved while Minecraft drives the camera.
		bool  fovSaved = false;
		float savedWorldFov = 0.0f;
		float savedFirstPersonFov = 0.0f;
		float cameraCheckTimer = 3.0f;
		std::atomic<bool> cameraCheckPending{ false };

		// Skyrim's FOV setting is the horizontal FOV of a 4:3 view; Minecraft's is vertical.
		float McFovToSkyrim(float a_mcVerticalDeg)
		{
			const float half = std::clamp(a_mcVerticalDeg, 10.0f, 170.0f) * 0.5f * kDegToRad;
			return 2.0f * std::atan(std::tan(half) * (4.0f / 3.0f)) * kRadToDeg;
		}

		void ApplyMcFov(RE::PlayerCamera* a_camera, float a_mcFov)
		{
			auto& data = a_camera->GetRuntimeData2();
			if (!fovSaved) {
				savedWorldFov = data.worldFOV;
				savedFirstPersonFov = data.firstPersonFOV;
				fovSaved = true;
				logger::info("camera FOV {} -> Minecraft {} (vertical {})", savedWorldFov, McFovToSkyrim(a_mcFov), a_mcFov);
			}
			data.worldFOV = McFovToSkyrim(a_mcFov);
		}

		void RestoreFov(RE::PlayerCamera* a_camera)
		{
			if (fovSaved && a_camera) {
				auto& data = a_camera->GetRuntimeData2();
				data.worldFOV = savedWorldFov;
				data.firstPersonFOV = savedFirstPersonFov;
				fovSaved = false;
			}
		}

		bool AnyBlockingMenuOpen(RE::UI* a_ui)
		{
			for (const auto& menu : a_ui->menuStack) {
				if (menu && menu->menuFlags.any(RE::UI_MENU_FLAGS::kPausesGame, RE::UI_MENU_FLAGS::kUsesCursor)) {
					return true;
				}
			}
			return false;
		}

		// Skyrim's NPCs only fight a player whose movement controls are on: their per-frame combat
		// update (AE ID 38565) is skipped while the player's are off, as in cutscenes. Saves keep
		// controls that were off when saving (older SkyCraft builds turned them off), so turn them
		// back on while Minecraft drives the player. Its input still never reaches Skyrim's player
		// (see Input.cpp's PlayerControls hook).
		void EnsureControlsEnabled()
		{
			auto* controls = RE::ControlMap::GetSingleton();
			if (!controls) {
				return;
			}
			using F = RE::ControlMap::UEFlag;
			constexpr auto kWanted = static_cast<std::uint32_t>(F::kMovement) | static_cast<std::uint32_t>(F::kLooking) |
			                         static_cast<std::uint32_t>(F::kActivate) | static_cast<std::uint32_t>(F::kFighting) |
			                         static_cast<std::uint32_t>(F::kSneaking) | static_cast<std::uint32_t>(F::kPOVSwitch) |
			                         static_cast<std::uint32_t>(F::kWheelZoom) | static_cast<std::uint32_t>(F::kJumping) |
			                         static_cast<std::uint32_t>(F::kVATS);
			const auto current = controls->GetRuntimeData().enabledControls.underlying();
			if ((current & kWanted) == kWanted) {
				return;
			}
			static std::uint32_t lastReported = 0xFFFFFFFF;
			if (current != lastReported) {
				lastReported = current;
				logger::info("Skyrim's player controls were off ({:#x}); turning them back on so NPCs fight", current);
			}
			controls->ToggleControls(static_cast<F>(kWanted & ~current), true, false);
		}

		// Skyrim's water surface over the block columns around Minecraft's player, so Minecraft
		// swims, floats and drowns in Skyrim's lakes and rivers.
		void WriteWaterGrid(RE::PlayerCharacter* a_player, const McVec& a_centre, std::uint32_t a_worldId)
		{
			auto* tes = RE::TES::GetSingleton();
			auto* cell = a_player->GetParentCell();
			if (!tes || !cell) {
				return;
			}
			static proto::WaterGrid grid{};
			constexpr int kSize = int(proto::kWaterGridSize);
			grid.originX = int(std::floor(a_centre.x)) - kSize / 2;
			grid.originZ = int(std::floor(a_centre.z)) - kSize / 2;
			grid.worldId = a_worldId;
			const float probeZ = float(a_centre.y * proto::kUnitsPerBlock);
			for (int dz = 0; dz < kSize; ++dz) {
				for (int dx = 0; dx < kSize; ++dx) {
					auto pos = McToSky(grid.originX + dx + 0.5, a_centre.y, grid.originZ + dz + 0.5);
					pos.z = probeZ;
					// The cell under that column (the player's own inside); TES::GetWaterHeight isn't
					// in 1.7.104's address library, the cell's own lookup is.
					auto* columnCell = cell->IsInteriorCell() ? cell : tes->GetCell(pos);
					float h = -std::numeric_limits<float>::max();
					if (columnCell) {
						columnCell->GetWaterHeight(pos, h);
					}
					// No water reads as a huge negative height (or NaN).
					grid.surface[dz * kSize + dx] = (std::isfinite(h) && h > -1.0e6f) ? float(h / proto::kUnitsPerBlock) : proto::kNoWater;
				}
			}
			Link::Get().WriteWaterGrid(grid);
		}

		// Hide Skyrim's HUD except the compass, the activation prompt and the enemy health bar
		// (Minecraft's HUD shows the player's own health, hunger and hotbar).
		void HideHud(RE::UI* a_ui, bool a_hide)
		{
			auto hud = a_ui->GetMenu(RE::HUDMenu::MENU_NAME);
			if (!hud || !hud->uiMovie) {
				return;
			}
			static constexpr const char* kElements[] = {
				"_root.HUDMovieBaseInstance.Health",
				"_root.HUDMovieBaseInstance.Magica",
				"_root.HUDMovieBaseInstance.Stamina",
				"_root.HUDMovieBaseInstance.LeftChargeMeter",
				"_root.HUDMovieBaseInstance.RightChargeMeter",
				"_root.HUDMovieBaseInstance.ArrowInfoInstance",
			};
			const RE::GFxValue visible(!a_hide);
			for (const auto* path : kElements) {
				hud->uiMovie->SetVariable((std::string(path) + "._visible").c_str(), visible);
			}
		}

		// Skyrim's crosshair stays gone while Minecraft drives (Minecraft's own is drawn instead).
		// Every frame, and see-through as well as hidden: Skyrim's HUD shows it again whenever it
		// updates it (menus closing, targets changing). The stealth eye shows only while sneaking.
		void HideCrosshair(RE::UI* a_ui, bool a_hide, bool a_sneaking)
		{
			auto hud = a_ui->GetMenu(RE::HUDMenu::MENU_NAME);
			if (!hud || !hud->uiMovie) {
				return;
			}
			static bool hidden = false;
			if (!a_hide && !hidden) {
				return;
			}
			hidden = a_hide;
			static constexpr const char* kCrosshair[] = {
				"_root.HUDMovieBaseInstance.CrosshairInstance",
				"_root.HUDMovieBaseInstance.CrosshairAlert",
			};
			const RE::GFxValue visible(!a_hide), alpha(a_hide ? 0.0 : 100.0);
			for (const auto* path : kCrosshair) {
				hud->uiMovie->SetVariable((std::string(path) + "._visible").c_str(), visible);
				hud->uiMovie->SetVariable((std::string(path) + "._alpha").c_str(), alpha);
			}
			hud->uiMovie->SetVariable("_root.HUDMovieBaseInstance.StealthMeterInstance._visible", RE::GFxValue(!a_hide || a_sneaking));
		}

		// Skyrim takes the player for its own animations: sitting, crafting stations, beds, pull-bar
		// levers and other furniture (it walks the player into place, plays the animation and uses its
		// furniture camera), riding a horse, kill moves, and scripted scenes that drive the player.
		// Minecraft lets go meanwhile (Skyrim gets the controls and the camera) and picks the player
		// up again from wherever Skyrim leaves them.
		const char* SkyrimTakeover(RE::PlayerCharacter* a_player)
		{
			if (a_player->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
				return "furniture";
			}
			if (a_player->IsOnMount()) {
				return "riding";
			}
			if (a_player->IsInKillMove()) {
				return "kill move";
			}
			// PlayerCharacter::SetAIDriven (AE 40586) sets bit 3 of this byte (decompiled, 1.7.104).
			if (REL::Module::IsAE() && (reinterpret_cast<const std::uint8_t*>(a_player)[0xBEA] & 8) != 0) {
				return "scripted scene";
			}
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->currentState) {
				switch (camera->currentState->id) {
				case RE::CameraState::kFurniture:
				case RE::CameraState::kAnimated:
				case RE::CameraState::kBleedout:
				case RE::CameraState::kDragon:
				case RE::CameraState::kMount:
				case RE::CameraState::kVATS:
					return "Skyrim's camera";
				default:
					break;
				}
			}
			return nullptr;
		}

		// Tells the player, in Skyrim's corner notifications, what Minecraft is up to while it isn't
		// connected: starting, waiting for the first-time Microsoft sign-in, or not set up at all.
		void ReportMinecraft(bool a_connected, bool a_inGame, float a_delta)
		{
			static float waited = 0.0f, nextNote = 3.0f;
			static bool  told = false, gaveUp = false, diagnosed = false;
			if (a_connected) {
				if (told) {
					RE::SendHUDMessage::ShowHUDMessage("SkyCraft: Minecraft is ready.");
				}
				waited = 0.0f;
				nextNote = 3.0f;
				told = gaveUp = diagnosed = false;
				return;
			}
			if (!a_inGame || gaveUp) {
				return;
			}
			waited += a_delta;
			if (waited < nextNote) {
				return;
			}
			const auto status = Launcher::GetStatus();
			const bool signIn = status == Launcher::Status::kSignIn;
			// A minute on and still nothing: say which of the ways it can be stuck this is.
			if (!diagnosed && waited > 60.0f && (status == Launcher::Status::kStarting || status == Launcher::Status::kRunning)) {
				diagnosed = true;
				const bool minecraft = Launcher::MinecraftRunning();
				const bool prism = Launcher::PrismRunning();
				logger::warn("Minecraft: not connected after a minute (Minecraft running: {}, Prism Launcher running: {})", minecraft, prism);
				RE::SendHUDMessage::ShowHUDMessage(
					minecraft ? "SkyCraft: Minecraft is running but not responding. Please report it with SkyCraft.log and Minecraft's latest.log." :
					prism     ? "SkyCraft: Minecraft hasn't started yet. Alt-Tab to Prism Launcher: it may still be downloading, or need a sign-in, or show an error." :
								"SkyCraft: Minecraft closed before connecting. Its log is AppData\\Local\\SkyCraft\\Prism\\instances\\SkyCraft\\.minecraft\\logs\\latest.log");
				told = true;
				nextNote = waited + 120.0f;
				return;
			}
			if (waited > (signIn ? 600.0f : 180.0f)) {
				RE::SendHUDMessage::ShowHUDMessage("SkyCraft: Minecraft still hasn't connected. See SkyCraft.log.");
				gaveUp = true;
				return;
			}
			const char* note = nullptr;
			switch (status) {
			case Launcher::Status::kSignIn:
				note = "SkyCraft: setting up Minecraft. Alt-Tab to the Prism window and sign in with your Microsoft account.";
				break;
			case Launcher::Status::kStarting:
			case Launcher::Status::kRunning:
				note = told ? nullptr : "SkyCraft: starting Minecraft...";
				break;
			case Launcher::Status::kNoLauncher:
				note = "SkyCraft: Minecraft isn't set up. See Data/SKSE/Plugins/SkyCraft.ini.";
				gaveUp = true;
				break;
			case Launcher::Status::kFailed:
				note = "SkyCraft: couldn't start Minecraft. See SkyCraft.log.";
				gaveUp = true;
				break;
			default:
				gaveUp = true;  // the player starts Minecraft themselves
				break;
			}
			if (note) {
				RE::SendHUDMessage::ShowHUDMessage(note);
				told = true;
			}
			nextNote = waited + 120.0f;
		}

		// Minecraft's crouch is Skyrim's sneak, so NPCs' detection, the stealth eye, sneak attacks
		// and the Sneak skill all follow it. Skyrim sneaks through its ActionSneak action, exactly
		// what its Sneak key does (SneakHandler -> PlayerControls::DoAction(kActionSneak, kTry),
		// AE 42350); it toggles, so it's sent whenever the two disagree, and again after a moment
		// if the animation graph didn't take it (mid-jump, say).
		void SyncSneak(RE::PlayerCharacter* a_player, bool a_mcSneaking, float a_delta)
		{
			static float retry = 0.0f;
			static int   logged = 0;
			retry -= a_delta;
			const auto* state = a_player->AsActorState();
			const bool  sneaking = state->actorState1.sneaking;
			if (sneaking == a_mcSneaking || retry > 0.0f || a_player->IsDead() || a_player->IsOnMount() || state->IsSwimming()) {
				return;
			}
			auto* controls = RE::PlayerControls::GetSingleton();
			if (!controls) {
				return;
			}
			using DoActionFn = bool (*)(RE::PlayerControls*, RE::DEFAULT_OBJECT, std::int32_t);
			static const auto doAction = reinterpret_cast<DoActionFn>(REL::ID(42350).address());
			const bool        ok = doAction(controls, RE::DEFAULT_OBJECT::kActionSneak, 2);
			retry = 0.3f;
			if (logged < 6) {
				++logged;
				logger::info("sneak: Minecraft {} crouching, Skyrim {} sneaking: toggled ({})", a_mcSneaking ? "is" : "isn't", sneaking ? "was" : "wasn't",
					ok ? "taken" : "refused");
			}
		}

		void PerFrame(RE::PlayerCharacter* a_player, float a_delta)
		{
			auto& st = State();
			auto& link = Link::Get();
			auto* ui = RE::UI::GetSingleton();
			link.Heartbeat();

			const bool mcAlive = link.McAlive();
			const bool haveMc = mcAlive && link.ReadMcState(mc);
			// A different Minecraft process (restarted while we were paused) counts as a reconnect too.
			static std::uint32_t lastMcPid = 0;
			const auto           mcPid = link.McPid();
			const bool           newMcProcess = mcAlive && mcPid != 0 && mcPid != lastMcPid;
			if (mcAlive) {
				lastMcPid = mcPid;
			}
			if (mcAlive && (!mcWasAlive || newMcProcess)) {
				// Minecraft (re)connected: resend all collision from a fresh epoch.
				logger::info("Minecraft connected");
				link.ResetOverlay();
				settleTimer = kSettleSeconds;
				++epoch;
				Collision::Get().Reset(epoch);
				teleportPending = true;
			}
			mcWasAlive = mcAlive;
			st.mcInWorld = haveMc && (mc.flags & proto::kMcInWorld);
			const bool screenOpen = haveMc && (mc.flags & proto::kMcScreenOpen);
			if (screenOpen && !st.mcScreenOpen) {
				st.cursorX = st.viewportW / 2;
				st.cursorY = st.viewportH / 2;
			}
			st.mcScreenOpen = screenOpen;
			if (haveMc && mc.sensitivity > 0.0f) {
				st.sensitivity = mc.sensitivity;
			}

			auto*      cell = a_player->GetParentCell();
			const bool loading = !cell || !a_player->Is3DLoaded() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
			const bool menu = AnyBlockingMenuOpen(ui);
			if ((menu || loading) && !st.skyrimMenuOpen) {
				Input::ReleaseAll();
			}
			st.skyrimMenuOpen = menu || loading;

			// World identity: exterior worldspace or interior cell. A change wipes MC's collision.
			if (cell) {
				auto*      ws = a_player->GetWorldspace();
				const auto id = cell->IsInteriorCell() ? cell->GetFormID() : (ws ? ws->GetFormID() : cell->GetFormID());
				if (id != worldId) {
					logger::info("world changed {:08X} -> {:08X}", worldId, id);
					worldId = id;
					Dig::SetWorld(id);
					++epoch;
					Collision::Get().Reset(epoch);
					teleportPending = true;
					settleTimer = kSettleSeconds;
				}
			}

			// Skyrim moved the player itself (load door, fast travel, script, loading a save).
			const auto current = a_player->GetPosition();
			if (loading) {
				teleportPending = true;
				haveLastSet = false;
				settleTimer = kSettleSeconds;
			} else if (haveLastSet && current.GetDistance(lastSetPos) > kSkyrimTeleportThreshold) {
				logger::info("Skyrim moved the player ({:.0f} units); resyncing Minecraft", current.GetDistance(lastSetPos));
				teleportPending = true;
				haveLastSet = false;
			}
			if (teleportPending && !loading) {
				++teleportSeq;
				teleportPending = false;
				st.yaw = HeadingToMcYaw(a_player->data.angle.z);
				st.pitch = a_player->data.angle.x * kRadToDeg;
				st.lookInitialized = true;
			}

			// Mouse look (MC's formula), integrated here so the Skyrim camera has zero added latency.
			float dx = 0.0f, dy = 0.0f;
			Input::ConsumeLook(dx, dy);
			if (!st.lookInitialized) {
				st.yaw = HeadingToMcYaw(a_player->data.angle.z);
				st.pitch = a_player->data.angle.x * kRadToDeg;
				st.lookInitialized = true;
			}
			if (!st.mcScreenOpen && !st.skyrimMenuOpen) {
				const float s = st.sensitivity * 0.6f + 0.2f;
				const float factor = s * s * s * 8.0f * 0.15f;
				st.yaw = std::fmod(st.yaw + dx * factor, 360.0f);
				st.pitch = std::clamp(st.pitch + dy * factor, -90.0f, 90.0f);
			}

			// Minecraft holds its player still after a teleport until Skyrim's ground has arrived
			// around them. If it's holding somewhere Skyrim's player isn't, that ground never comes:
			// send it again to where Skyrim's player really is.
			static const char* takeover = nullptr;
			const char*        takeoverNow = a_player->IsDead() ? nullptr : SkyrimTakeover(a_player);
			if ((takeoverNow != nullptr) != (takeover != nullptr)) {
				if (takeoverNow) {
					logger::info("Skyrim takes the player ({})", takeoverNow);
				} else {
					logger::info("Skyrim hands the player back");
					teleportPending = true;  // Minecraft picks up wherever Skyrim left the player
				}
			}
			takeover = takeoverNow;
			const bool arriving = haveMc && st.mcInWorld && !loading && mc.teleportAck != teleportSeq && !takeover;
			if (arriving) {
				const auto   here = SkyToMc(current);
				const double gap = std::sqrt((here.x - mc.x) * (here.x - mc.x) + (here.y - mc.y) * (here.y - mc.y) + (here.z - mc.z) * (here.z - mc.z));
				holdMismatch = gap > 8.0 ? holdMismatch + a_delta : 0.0f;
				if (holdMismatch > 1.0f) {
					logger::info("Minecraft is waiting {:.0f} blocks from Skyrim's player; teleporting it again", gap);
					teleportPending = true;
					holdMismatch = 0.0f;
				}
			} else {
				holdMismatch = 0.0f;
			}

			// A dead Skyrim player gets Skyrim's own death camera and reload.
			const bool puppet = haveMc && st.mcInWorld && mc.teleportAck == teleportSeq && !loading && !a_player->IsDead() && !takeover;
			st.minecraftOwnsPlayer = puppet || (arriving && !a_player->IsDead());
			if (puppet != st.puppeting) {
				logger::info("puppet {}", puppet ? "on (Minecraft drives the player)" : "off");
			}
			st.puppeting = puppet;
			if (puppet) {
				EnsureControlsEnabled();
				Game::NoteFrameStep('P');
				SyncSneak(a_player, (mc.flags & proto::kMcSneaking) != 0, a_delta);
			}
			if (ui) {
				HideCrosshair(ui, puppet, a_player->AsActorState()->actorState1.sneaking);
			}
			st.mcCrosshair = puppet && mc.cameraMode == 0 && !st.mcScreenOpen && !st.skyrimMenuOpen;
			st.mcGuiScale = haveMc ? static_cast<int>(mc.guiScale) : 0;
			Input::SetActivatePromptKey(puppet);
			Combat::PerFrame(a_player, puppet, a_delta);
			WorldRender::UpdateRagdoll(a_player, haveMc && st.mcInWorld);
			if (puppet) {
				NpcBlocks::PushActorsOut(a_player, a_delta);
			}

			// Interpolate Minecraft's 20 Hz physics ticks on our own clock (what MC's renderer does
			// with partial ticks). Sampling MC's per-frame position instead judders, because the two
			// games' frames aren't phase-locked.
			double feetX = mc.x, feetY = mc.y, feetZ = mc.z;
			double eyeX = mc.eyeX, eyeY = mc.eyeY, eyeZ = mc.eyeZ;
			float  bobPhaseNow = mc.bobPhase, bobAmountNow = mc.bobAmount;
			if (mc.tickQpc != 0 && mc.tickMs > 0.0f) {
				static const std::int64_t qpcFreq = [] { LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return f.QuadPart; }();
				const double       qpcPerMs = double(qpcFreq) / 1000.0;
				const std::int64_t period = std::max<std::int64_t>(1, std::llround(double(mc.tickMs) * qpcPerMs));
				LARGE_INTEGER      now;
				::QueryPerformanceCounter(&now);
				// Keep a short history of Minecraft's ticks and render slightly in the past, so the
				// next tick has always arrived before we need it: pure interpolation (Minecraft's own
				// model), never extrapolation, so no hitches when velocity changes (jumps, landings).
				if (tickHistory.empty() || tickHistory.back().s.tickQpc != mc.tickQpc) {
					if (!tickHistory.empty() && mc.tickQpc < tickHistory.back().s.tickQpc) {
						tickHistory.clear();  // Minecraft restarted
					}
					Tick tick{ mc, mc.tickQpc, 1 };
					if (!tickHistory.empty()) {
						auto&              last = tickHistory.back();
						const std::int64_t n = std::llround(double(mc.tickQpc - last.at) / double(period));
						const std::int64_t err = mc.tickQpc - (last.at + n * period);
						if (n == 0 && last.slots >= 2) {
							// Minecraft ran two ticks in one frame and we saw both: the first one
							// carries the second's stamp. It belongs a tick earlier.
							last.at -= period;
							last.slots -= 1;
							tick.at = last.at + period;
						} else if (n >= 1 && n <= 10 && std::abs(err) < period * 3 / 10) {
							tick.at = last.at + n * period + err / 16;  // the rhythm is exact; the stamps are noisy
							tick.slots = static_cast<int>(n);
							stampOutliers = 0;
							motion.stampErrMs += std::abs(double(err)) / qpcPerMs;
							++motion.stampSamples;
						} else if (n <= 10 && ++stampOutliers < 3) {
							tick.slots = static_cast<int>(std::max<std::int64_t>(n, 1));
							tick.at = last.at + tick.slots * period;  // one odd stamp (a hitch): keep the rhythm
						} else {
							stampOutliers = 0;  // lost the rhythm (a pause, a new tick rate): start from this stamp
						}
					}
					// Was it already due on our last frame? Then rendering had to wait for it.
					if (lastFrameQpc != 0) {
						if (!tickDueInit) {
							tickDue.fill(renderDelayMs - 1.0);
							tickDueInit = true;
						}
						const double dueMs = double(lastFrameQpc - tick.at) / qpcPerMs;
						if (dueMs < 30.0) {  // later than that is a hitch, not a pattern to wait for
							tickDue[tickDueNext++ % tickDue.size()] = dueMs;
						}
					}
					// Auto-capture a jump (rising faster than walking up a slope), or a climb in third person.
					const double rise = mc.curY - mc.prevY, run = std::hypot(mc.curX - mc.prevX, mc.curZ - mc.prevZ);
					dbgClimbTicks = mc.cameraMode != 0 && (mc.flags & proto::kMcOnGround) && rise > 0.03 && run > 0.1 ? dbgClimbTicks + 1 : 0;
					if (!dbgJumpTrigger && ((rise > 0.3 && dbgSinceJumpCapture > 6.0f) || (dbgClimbTicks >= 10 && dbgSinceJumpCapture > 30.0f))) {
						dbgJumpTrigger = true;
						dbgTriggerReason = rise > 0.3 ? "jump" : "climbing in third person";
						dbgSinceJumpCapture = 0.0f;
					}
					tickHistory.push_back(tick);
					if (tickHistory.size() > 8) {
						tickHistory.pop_front();
					}
					++motion.ticks;
				}
				dbgSinceJumpCapture += a_delta;

				// The render delay follows how late ticks have been over the last 2 s: it grows 2%
				// slower than real time and shrinks 0.2% faster, too little to see either way.
				const double frameMs = lastFrameQpc != 0 ? double(now.QuadPart - lastFrameQpc) / qpcPerMs : 0.0;
				lastFrameQpc = now.QuadPart;
				if (tickDueInit) {
					const double target = std::clamp(*std::ranges::max_element(tickDue) + 1.0, 4.0, 30.0);
					const double dt = std::min(frameMs, 100.0) / 1000.0;
					renderDelayMs = target > renderDelayMs ? std::min(target, renderDelayMs + 20.0 * dt) : std::max(target, renderDelayMs - 2.0 * dt);
				}
				const std::int64_t renderQpc = now.QuadPart - std::llround(renderDelayMs * qpcPerMs);

				// The feet go through each tick's start (prev) and end (cur) positions.
				std::size_t i = 0;
				for (std::size_t k = tickHistory.size(); k-- > 0;) {
					if (tickHistory[k].at <= renderQpc) {
						i = k;
						break;
					}
				}
				const Tick&  tick = tickHistory[i];
				const Tick*  next = i + 1 < tickHistory.size() ? &tickHistory[i + 1] : nullptr;
				const double ticks = double(renderQpc - tick.at) / double(period);
				const double t = std::clamp(ticks, 0.0, 1.0);
				dbgT = ticks;
				dbgTickQpc = tick.s.tickQpc;
				const auto&  s = tick.s;
				feetX = s.prevX + (s.curX - s.prevX) * t;
				feetY = s.prevY + (s.curY - s.prevY) * t;
				feetZ = s.prevZ + (s.curZ - s.prevZ) * t;
				double eyeHeight = s.tickEyeO + (s.tickEye - s.tickEyeO) * t;
				bobPhaseNow = -(s.walkDist + (s.walkDist - s.walkDistO) * static_cast<float>(t));
				bobAmountNow = s.bobO + (s.bob - s.bobO) * static_cast<float>(t);
				if (ticks > 1.0 && next) {
					// Past this tick's end, and the next tick we have starts later: Minecraft ran one
					// we never saw. Carry on from this tick's end to the next one's start.
					const auto&  n = next->s;
					const double gap = double(next->at - (tick.at + period));
					const double u = gap > 0.0 ? std::clamp(double(renderQpc - (tick.at + period)) / gap, 0.0, 1.0) : 1.0;
					feetX = s.curX + (n.prevX - s.curX) * u;
					feetY = s.curY + (n.prevY - s.curY) * u;
					feetZ = s.curZ + (n.prevZ - s.curZ) * u;
					eyeHeight = s.tickEye + (n.tickEyeO - s.tickEye) * u;
					const float endPhase = -(s.walkDist + (s.walkDist - s.walkDistO));
					bobPhaseNow = endPhase + (-n.walkDist - endPhase) * static_cast<float>(u);
					bobAmountNow = s.bob + (n.bobO - s.bob) * static_cast<float>(u);
				} else if (ticks > 1.0) {
					++motion.lateFrames;  // the next tick hasn't arrived: the player stands still this frame
				}
				eyeX = feetX;
				eyeY = feetY + eyeHeight;
				eyeZ = feetZ;
				++motion.frames;
				motion.frameMsSum += frameMs;
				motion.frameMsMax = std::max(motion.frameMsMax, frameMs);
			}

			if (puppet) {
				const auto pos = McToSky(feetX, feetY, feetZ);
				a_player->SetPosition(pos, true);
				if (auto* controller = a_player->GetCharController()) {
					// Minecraft moves the player; Skyrim keeps no momentum or fall damage of its own.
					// Its gravity stays on so the body keeps ground contact between our updates:
					// Skyrim's AI won't melee a target it thinks is in the air.
					controller->SetLinearVelocityImpl(RE::hkVector4(0.0f, 0.0f, 0.0f, 0.0f));
					if (savedGravity >= 0.0f) {
						controller->gravity = savedGravity;  // undo the zero-gravity of older builds
						savedGravity = -1.0f;
					}
					controller->fallStartHeight = pos.z;
					controller->fallTime = 0.0f;
					if (mc.flags & proto::kMcOnGround) {
						controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
						controller->flags.set(RE::CHARACTER_FLAGS::kSupport);
					} else {
						controller->context.currentState = RE::hkpCharacterStateType::kInAir;
					}
				}
				lastSetPos = pos;
				haveLastSet = true;
				{
					auto* proxyController = skyrim_cast<RE::bhkCharProxyController*>(a_player->GetCharController());
					auto* proxy = proxyController ? proxyController->GetCharacterProxy() : nullptr;
					Dig::SetPuppet(proxy && proxy->shapePhantom ? &proxy->shapePhantom->collidable : nullptr, pos.z * RE::bhkWorld::GetWorldScale());
				}

				// Minecraft's walk bob (GameRenderer.bobView), converted from a view-space pose on
				// the scene into the equivalent camera offset: sway sideways, lift, and dip the view.
				const float  phase = bobPhaseNow * 3.14159265f;
				const float  bob = bobAmountNow;
				const float  sideBlocks = -std::sin(phase) * bob * 0.5f;
				const float  liftBlocks = std::fabs(std::cos(phase) * bob);
				const float  bobPitchDeg = std::fabs(std::cos(phase - 0.2f) * bob) * 5.0f;
				const float  heading = McYawToHeading(st.yaw);
				const RE::NiPoint3 right{ std::cos(heading), -std::sin(heading), 0.0f };
				eyePos = McToSky(eyeX, eyeY, eyeZ);
				dbgFeet = McToSky(feetX, feetY, feetZ);
				dbgLift = liftBlocks * static_cast<float>(proto::kUnitsPerBlock);
				dbgSide = sideBlocks * static_cast<float>(proto::kUnitsPerBlock);
				eyePos += right * (sideBlocks * static_cast<float>(proto::kUnitsPerBlock));
				eyePos.z += liftBlocks * static_cast<float>(proto::kUnitsPerBlock);
				eyeValid = true;
				st.feetX = feetX, st.feetY = feetY, st.feetZ = feetZ;
				st.feetValid = true;

				// Minecraft's F5 camera: behind the player, or in front looking back at them, pulled
				// in wherever Minecraft's own zoom collision stopped it (its blocks and Skyrim's
				// triangles). Skyrim stays in its first-person state; only the camera moves.
				const bool  mirrored = mc.cameraMode == 2;
				const float camHeading = McYawToHeading(mirrored ? st.yaw + 180.0f : st.yaw);
				const float lookPitchDeg = mirrored ? -st.pitch : st.pitch;
				const float camPitchDeg = lookPitchDeg + bobPitchDeg;
				// Minecraft's zoom pulls in the moment something is behind the player (never
				// through a wall) and eases back out, so a ray grazing the ground can't shake it.
				const bool detached = mc.cameraMode != 0 && mc.cameraDistance > 0.0f;
				const float zoomBefore = zoom;
				if (!detached || zoomMode != mc.cameraMode || mc.cameraDistance < zoom) {
					zoom = detached ? mc.cameraDistance : 0.0f;
				} else {
					zoom += (mc.cameraDistance - zoom) * (1.0f - std::exp(-std::max(a_delta, 0.0f) / 0.2f));
				}
				zoomMode = mc.cameraMode;
				motion.zoomTravel += std::fabs(zoom - zoomBefore);
				{
					static float lastYaw = st.yaw;
					float        turn = std::fmod(st.yaw - lastYaw + 540.0f, 360.0f) - 180.0f;
					lastYaw = st.yaw;
					std::lock_guard lock(orbitLock);
					orbitPivot = eyePos;
					orbitUnits = zoom * static_cast<float>(proto::kUnitsPerBlock);
					orbitValid = detached;
					orbitYawSpeed = a_delta > 0.0f ? turn / a_delta : 0.0f;
				}
				if (detached) {
					// Along the look direction without the walk bob: Minecraft tilts the view for
					// the bob around the camera itself, not by swinging the camera around the head.
					const auto forward = Col(BuildCameraRotation(camHeading, lookPitchDeg * kDegToRad, 0.0f), 0);
					eyePos -= forward * (zoom * static_cast<float>(proto::kUnitsPerBlock));
				}
				// Skyrim treats a player whose weapons are away as yielding: guards and townsfolk stop
				// fighting and wait to talk instead. So the Skyrim player's weapons come out while
				// anyone is fighting them, and go away after. Its first-person arms and weapons stay
				// hidden meanwhile (Minecraft shows the hands).
				const auto weapons = a_player->AsActorState()->GetWeaponState();
				const bool fighting = Combat::PlayerEngaged();
				// Always hidden while Minecraft drives (Minecraft draws the hands). Showing them again
				// after a fight un-hid arms Skyrim had hidden itself meanwhile (putting weapons away),
				// and they then floated in view until the model was rebuilt.
				HideFirstPersonMeshes(a_player, true);

				const float bobRollDeg = std::sin(phase) * bob * 3.0f;
				idealRotNoRoll = BuildCameraRotation(camHeading, camPitchDeg * kDegToRad, 0.0f);
				idealRot = BuildCameraRotation(camHeading, camPitchDeg * kDegToRad, bobRollDeg * kDegToRad);
				idealValid = !st.skyrimMenuOpen;

				// Place and turn the camera together, here. Skyrim's camera update (where it's also
				// done, see PlayerCameraUpdateHook) runs before this in the frame, so on its own it
				// would leave the camera facing last frame's way: a third-person camera placed for
				// this frame's turn but facing the previous one swings the view off the player while
				// turning. Putting it at Minecraft's eye up front also keeps Skyrim's first-person
				// camera easing (~50 ms) from starting anywhere else.
				if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
					auto* root = camera->cameraRoot.get();
					static bool loggedParent = false;
					if (!loggedParent) {
						loggedParent = true;
						logger::info("camera root parent: {}", root->parent ? root->parent->name.c_str() : "(none)");
					}
					if (idealValid) {
						ApplyLookRotation(root);
					}
					PinCameraAndSky();
					RE::NiUpdateData update{};
					root->UpdateDownwardPass(update, 0);
				}

				motionLogTimer -= a_delta;
				if (motionLogTimer <= 0.0f) {
					motionLogTimer = 10.0f;
					if (motion.frames > 0 && DiagnosticsEnabled()) {
						logger::info("motion: {} ticks, {} of {} frames waited for a late tick, render delay {:.1f} ms, tick stamp noise {:.2f} ms, frames {:.1f} ms avg {:.1f} max, zoom moved {:.2f} blocks{}",
							motion.ticks, motion.lateFrames, motion.frames, renderDelayMs, motion.stampSamples ? motion.stampErrMs / motion.stampSamples : 0.0,
							motion.frameMsSum / motion.frames, motion.frameMsMax, motion.zoomTravel, mc.cameraMode != 0 ? " (third person)" : "");
					}
					motion = {};
					std::lock_guard lock(orbitLock);
					if (orbit.frames > 0 && DiagnosticsEnabled()) {
						logger::info("third-person orbit: drawn camera off the orbit around the eye by {:.1f} units avg, {:.1f} max ({:.1f} avg over {} frames turning); drawn camera vs this frame's camera {:.1f} avg, {:.1f} max, over {} frames",
							orbit.missSum / orbit.frames, orbit.missMax, orbit.turning ? orbit.turnMissSum / orbit.turning : 0.0f, orbit.turning,
							orbit.staleSum / orbit.frames, orbit.staleMax, orbit.frames);
					}
					orbit = {};
				}

				devLogTimer -= a_delta;
				if (devLogTimer <= 0.0f && devCount > 0 && DiagnosticsEnabled()) {
					devLogTimer = 60.0f;
					logger::info("Skyrim's own camera rotation vs Minecraft look: mean {:.2f} deg, max {:.2f} deg over {} frames{}",
						devSum / devCount, devMax, devCount, rotValidated ? " (overridden)" : "");
					devSum = devMax = 0.0f;
					devCount = 0;
				}

				if (!st.skyrimMenuOpen) {
					a_player->data.angle.z = heading;
					a_player->data.angle.x = (st.pitch + bobPitchDeg) * kDegToRad;
					if (auto* camera = RE::PlayerCamera::GetSingleton()) {
						if (!camera->IsInFirstPerson()) {
							camera->ForceFirstPerson();
						}
						if (mc.fovDeg > 1.0f) {
							ApplyMcFov(camera, mc.fovDeg);
						}
						cameraCheckTimer -= a_delta;
						if (cameraCheckTimer <= 0.0f) {
							cameraCheckTimer = 10.0f;
							cameraCheckPending = true;  // measured in Present, after this frame rendered
						}
					}
					if (fighting && weapons == RE::WEAPON_STATE::kSheathed) {
						a_player->DrawWeaponMagicHands(true);
					} else if (!fighting && weapons == RE::WEAPON_STATE::kDrawn) {
						a_player->DrawWeaponMagicHands(false);
					}
				}
			} else {
				eyeValid = false;
				idealValid = false;
				st.feetValid = false;
				Dig::SetPuppet(nullptr, 0.0f);
				HideFirstPersonMeshes(a_player, false);

				RestoreFov(RE::PlayerCamera::GetSingleton());
				if (auto* controller = a_player->GetCharController(); controller && savedGravity >= 0.0f) {
					controller->gravity = savedGravity;
					savedGravity = -1.0f;
				}
			}

			captureTimer -= a_delta;
			hudTimer -= a_delta;
			if (hudTimer <= 0.0f) {
				hudTimer = 0.5f;
				HideHud(ui, puppet);
			}

			// Tell Minecraft where Skyrim's player is and where they're looking.
			proto::SkyState sky{};
			sky.flags = (cell ? proto::kSkyInGame : 0u) | (menu ? proto::kSkyMenuOpen : 0u) | (loading ? proto::kSkyLoading : 0u);
			const auto skyMc = SkyToMc(current);
			sky.worldId = worldId;
			sky.collisionEpoch = epoch;
			sky.posX = skyMc.x;
			sky.posY = skyMc.y;
			sky.posZ = skyMc.z;
			sky.yaw = st.yaw;
			sky.pitch = st.pitch;
			sky.teleportSeq = teleportSeq;
			sky.viewportW = static_cast<std::uint32_t>(st.viewportW.load());
			sky.viewportH = static_cast<std::uint32_t>(st.viewportH.load());
			if (auto* calendar = RE::Calendar::GetSingleton()) {
				sky.gameHour = calendar->GetHour();
			}
			link.WriteSkyState(sky);

			settleTimer -= a_delta;
			if (haveMc && !loading && cell && settleTimer <= 0.0f) {
				Perf::Scope timer(Perf::kCollision);
				Collision::Get().Update(puppet ? McVec{ mc.x, mc.y, mc.z } : skyMc);
			}

			static int waterFrame = 0;
			if (haveMc && st.mcInWorld && !loading && cell && ++waterFrame % 3 == 0) {
				Perf::Scope timer(Perf::kWater);
				WriteWaterGrid(a_player, puppet ? McVec{ mc.x, mc.y, mc.z } : skyMc, worldId);
			}

			// Minecraft's torches, lava and glowstone light Skyrim's world while Minecraft is there.
			const McVec lightCentre = puppet ? McVec{ mc.x, mc.y, mc.z } : skyMc;
			{
				Perf::Scope timer(Perf::kLights);
				BlockLights::Update(haveMc && st.mcInWorld && !loading && cell ? &lightCentre : nullptr, a_delta);
			}

			// Blocks dug out of Skyrim's world: its collision around them goes to Minecraft again, and
			// its meshes are cut.
			static std::vector<Clip::Cube> dugChanged;
			dugChanged.clear();
			Dig::TakeChanged(dugChanged);
			if (!dugChanged.empty()) {
				Perf::Scope timer(Perf::kDigChanged);
				Collision::Get().DigChanged(dugChanged);
			}
			// Not while the world is still settling after a load (its cells are still being attached).
			static float digSettled = 0.0f;
			digSettled = puppet && !loading && cell ? digSettled + a_delta : 0.0f;
			{
				Perf::Scope timer(Perf::kDigMeshes);
				Dig::UpdateMeshes(digSettled > 3.0f ? a_player : nullptr, a_delta, !dugChanged.empty());
			}

			ReportMinecraft(mcAlive, cell && !loading && !menu, a_delta);
		}

		struct PlayerUpdateHook
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				try {
					Perf::Scope timer(Perf::kUpdate);
					PerFrame(a_this, a_delta);
				} catch (const std::exception& e) {
					logger::error("per-frame update: {}", e.what());
				}
				Perf::Report();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		std::atomic<std::uint64_t> postCameraCalls{ 0 };



		// Runs after Skyrim's whole camera update: re-pin the camera anchor and the sky to
		// Minecraft's eye so nothing downstream uses Skyrim's own smoothed camera position.
		struct PlayerCameraUpdateHook
		{
			static void thunk(RE::PlayerCamera* a_this)
			{
				func(a_this);
				++postCameraCalls;
				Game::NoteFrameStep('H');
				if (!eyeValid || !a_this->cameraRoot) {
					return;
				}
				auto* root = a_this->cameraRoot.get();
				if (idealValid) {
					const auto& R = root->world.rotate;
					// Candidates: +f, -f, +u, -u, +r, -r from our pure look direction (no roll).
					const RE::NiPoint3 f = Col(idealRotNoRoll, 0), u = Col(idealRotNoRoll, 1), r = Col(idealRotNoRoll, 2);
					const std::array<RE::NiPoint3, 6> cand{ f, f * -1.0f, u, u * -1.0f, r, r * -1.0f };
					if (!rotValidated && !rotRejected) {
						for (int c = 0; c < 3; ++c) {
							int   best = -1;
							float bestAngle = 1e9f;
							for (int k = 0; k < 6; ++k) {
								const float a = AngleDeg(Col(R, c), cand[k]);
								if (a < bestAngle) {
									bestAngle = a;
									best = k;
								}
							}
							if (bestAngle < 6.0f) {
								++axisVotes[c][best];
							}
						}
						if (++axisSamples >= 240) {
							bool ok = true;
							std::array<bool, 3> used{};
							for (int c = 0; c < 3; ++c) {
								const auto it = std::ranges::max_element(axisVotes[c]);
								const int  k = static_cast<int>(it - axisVotes[c].begin());
								ok &= *it > axisSamples * 6 / 10 && !used[k / 2];
								used[k / 2] = true;
								axisMap[c] = k;
							}
							static constexpr const char* kNames[6] = { "+forward", "-forward", "+up", "-up", "+right", "-right" };
							rotValidated = ok;
							rotRejected = !ok;
							logger::info("camera root axes: col0={} col1={} col2={} -> {}", kNames[axisMap[0]], kNames[axisMap[1]], kNames[axisMap[2]],
								ok ? "Minecraft now drives camera rotation" : "inconsistent; leaving rotation to Skyrim");
						}
					}
					// Deviation of Skyrim's own (animated) camera from the pure look direction.
					float dev = 0.0f;
					for (int c = 0; c < 3; ++c) {
						dev = std::max(dev, AngleDeg(Col(R, c), cand[axisMap[c]]));
					}
					dbgRotDev = dev;
					devMax = std::max(devMax, dev);
					devSum += dev;
					++devCount;
					ApplyLookRotation(root);
				}
				PinCameraAndSky();
				// Push the final transform down to the render camera (NiCamera child).
				RE::NiUpdateData update{};
				root->UpdateDownwardPass(update, 0);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// First-person camera position = Minecraft's eye (includes sneaking height etc.).
		struct FirstPersonTranslationHook
		{
			static void thunk(RE::TESCameraState* a_this, RE::NiPoint3& a_out)
			{
				func(a_this, a_out);
				if (eyeValid) {
					a_out = eyePos;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace Game
	{
		// High-rate capture: every 20 s, record 120 consecutive frames of where things really are.
		struct CaptureRow
		{
			std::int64_t qpc;
			RE::NiPoint3 eye, root, render, sky, actor, feet;
			double       t;
			std::int64_t tickQpc;
			float        lift, side, rotDev;
			float        delay, zoom;
		};
		std::vector<CaptureRow> capture;

		void CaptureFrame()
		{
			if (!eyeValid) {
				return;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			auto* renderCam = RE::Main::WorldRootCamera();
			auto* sky = RE::Sky::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!camera || !camera->cameraRoot || !renderCam || !player) {
				return;
			}
			LARGE_INTEGER now;
			::QueryPerformanceCounter(&now);
			capture.push_back({ now.QuadPart, eyePos, camera->cameraRoot->world.translate, renderCam->world.translate,
				sky && sky->root ? sky->root->world.translate : RE::NiPoint3{}, player->GetPosition(), dbgFeet, dbgT, dbgTickQpc, dbgLift, dbgSide, dbgRotDev, float(renderDelayMs), zoom });
			if (capture.size() < 240) {
				return;
			}
			LARGE_INTEGER f;
			::QueryPerformanceFrequency(&f);
			logger::info("frame capture: dt | t newtick | feet step (xy, z) | eye step | bob lift side | sky | delay zoom; post-camera hook calls: {}", postCameraCalls.load());
			for (std::size_t i = 0; i < capture.size(); ++i) {
				const auto& r = capture[i];
				const auto& p = i ? capture[i - 1] : r;
				const double ms = double(r.qpc - p.qpc) * 1000.0 / double(f.QuadPart);
				const float  fxy = std::hypot(r.feet.x - p.feet.x, r.feet.y - p.feet.y);
				logger::info("  {:3} {:5.2f}ms | t {:5.3f} {} | feet {:5.2f} {:6.2f} | eye {:5.2f} | bob {:5.2f} {:5.2f} | sky {:4.1f} | {:4.1f}ms {:5.3f}",
					i, ms, r.t, r.tickQpc != p.tickQpc ? "NEW" : "   ", fxy, r.feet.z - p.feet.z, r.eye.GetDistance(p.eye), r.lift, r.side, r.sky.GetDistance(r.eye), r.delay, r.zoom);
			}
			capture.clear();
			captureTimer = 20.0f;
		}

		void NoteFrameStep(char a_step)
		{
			if (frameOrderLogged || !State().puppeting || !DiagnosticsEnabled()) {
				return;
			}
			std::lock_guard lock(orbitLock);
			frameOrder += a_step;
			if (frameOrder.size() >= 60) {
				frameOrderLogged = true;
				logger::info("frame order (P player update, H camera update, D block drawing, X present): {}", frameOrder);
			}
		}

		void NoteRenderedCamera(const RE::NiPoint3& a_pos, const RE::NiMatrix3& a_rot)
		{
			std::lock_guard lock(orbitLock);
			if (!orbitValid) {
				return;
			}
			// Where the camera would be if it orbited the eye along the direction it's looking.
			const RE::NiPoint3 forward{ a_rot.entry[0][0], a_rot.entry[1][0], a_rot.entry[2][0] };
			const float        miss = a_pos.GetDistance(orbitPivot - forward * orbitUnits);
			const float        stale = a_pos.GetDistance(eyePos);
			++orbit.frames;
			orbit.missSum += miss;
			orbit.missMax = std::max(orbit.missMax, miss);
			orbit.staleSum += stale;
			orbit.staleMax = std::max(orbit.staleMax, stale);
			if (std::fabs(orbitYawSpeed) > 60.0f) {
				++orbit.turning;
				orbit.turnMissSum += miss;
			}
		}

		void CheckRenderedCamera()
		{
			NoteFrameStep('X');
			if (!DiagnosticsEnabled()) {
				return;
			}
			if (dbgJumpTrigger && capture.empty()) {
				dbgJumpTrigger = false;
				captureTimer = 0.0f;
				logger::info("{}; capturing", dbgTriggerReason);
			}
			if (captureTimer <= 0.0f || !capture.empty()) {
				CaptureFrame();
			}
			if (!cameraCheckPending.exchange(false) || !eyeValid) {
				return;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !camera->cameraRoot) {
				return;
			}
			const auto& cam = camera->cameraRoot->world.translate;
			logger::info("camera check (same frame): rendered ({:.1f} {:.1f} {:.1f}) vs Minecraft eye ({:.1f} {:.1f} {:.1f}), off by {:.1f} units",
				cam.x, cam.y, cam.z, eyePos.x, eyePos.y, eyePos.z, cam.GetDistance(eyePos));
			if (auto* sky = RE::Sky::GetSingleton(); sky && sky->root) {
				const auto& s = sky->root->world.translate;
				logger::info("  sky root ({:.1f} {:.1f} {:.1f}), {:.1f} units from the camera", s.x, s.y, s.z, s.GetDistance(cam));
			}
		}

		void Install()
		{
			REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerCharacter[0] };
			PlayerUpdateHook::func = playerVtbl.write_vfunc(0xAD, PlayerUpdateHook::thunk);

			// PlayerCamera::Update is called directly (not through the vtable), so hook its call
			// sites: scan the game's code for `call PlayerCamera::Update` and redirect each one.
			{
				const auto target = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(49852, 50784) }.address();
				const auto text = REL::Module::get().segment(REL::Segment::textx);
				const auto base = text.address();
				const auto* code = reinterpret_cast<const std::uint8_t*>(base);
				std::vector<std::uintptr_t> sites;
				for (std::size_t i = 0; i + 5 <= text.size(); ++i) {
					if (code[i] != 0xE8) {
						continue;
					}
					std::int32_t rel;
					std::memcpy(&rel, code + i + 1, 4);
					if (base + i + 5 + static_cast<std::intptr_t>(rel) == target) {
						sites.push_back(base + i);
					}
				}
				auto& trampoline = SKSE::GetTrampoline();
				for (const auto site : sites) {
					PlayerCameraUpdateHook::func = trampoline.write_call<5>(site, PlayerCameraUpdateHook::thunk);
				}
				logger::info("PlayerCamera::Update: hooked {} call site(s)", sites.size());
			}

			REL::Relocation<std::uintptr_t> fpVtbl{ RE::VTABLE_FirstPersonState[0] };
			FirstPersonTranslationHook::func = fpVtbl.write_vfunc(0x5, FirstPersonTranslationHook::thunk);

			Collision::Get().Start();
			Combat::Install();
			logger::info("game hooks installed");
		}

		bool SkyrimMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && AnyBlockingMenuOpen(ui);
		}

		void OnGameLoaded()
		{
			teleportPending = true;
			haveLastSet = false;
			State().lookInitialized = false;
		}
	}
}
