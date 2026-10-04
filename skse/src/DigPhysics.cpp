#include "Dig.h"
#include "Game.h"
#include "Perf.h"

// Skyrim's NPCs in a dug-up world. Every actor moves with a Havok character proxy: it collects
// contact points with the world (bhkCharacterPointCollector), turns them into surface planes, and
// solves its movement against those planes. Two hooks:
//  - contact points on diggable Skyrim geometry inside a dug block are dropped, so the ground
//    that was dug away holds nobody up any more (they fall into holes);
//  - Minecraft's solid blocks are added as surface planes, so NPCs stand on them (the floor of a
//    hole, a pillar, a bridge) and bump into them, like any Skyrim surface.
// Reverse-engineered on 1.7.104: hkpCharacterProxy::integrate (0x140B9E4E0) and checkSupport
// (0x140B9D340) both size the solver's plane arrays for the manifold plus the proxy's userPlanes
// (+0xB4) extra planes, then call every listener's processConstraintsCallback (0x140BA0990), which
// may add up to userPlanes planes. Skyrim's own surface planes (0x140BA04D0): the contact normal,
// distance minus keepDistance (+0xA8), the proxy's frictions, and a push out of penetration at
// penetrationRecoverySpeed (+0xDC).
namespace skycraft::Dig
{
	namespace
	{
		constexpr float kIntoSurface = 0.05f;   // blocks: a contact point belongs to the block behind it
		constexpr float kPlaneReach = 0.6f;     // Havok units: blocks this close to the proxy become planes
		constexpr int   kWantUserPlanes = 16;   // planes each proxy may take (Havok's default: 4)

		float BlocksPerHavok()
		{
			return RE::bhkWorld::GetWorldScaleInverse() / static_cast<float>(proto::kUnitsPerBlock);
		}

		const RE::hkpCollidable* Root(const RE::hkpCdBody* a_body)
		{
			while (a_body && a_body->parent) {
				a_body = a_body->parent;
			}
			return static_cast<const RE::hkpCollidable*>(a_body);
		}

		bool IsCharacter(const RE::hkpCollidable* a_collidable)
		{
			return a_collidable && a_collidable->GetCollisionLayer() == RE::COL_LAYER::kCharController;
		}

		// Is this contact on diggable Skyrim geometry in a dug block?
		bool OnDugGround(const RE::hkpCdPoint& a_point)
		{
			const auto* rootA = Root(a_point.cdBodyA);
			const auto* rootB = Root(a_point.cdBodyB);
			// The world object is B for a character's own queries; allow either way round.
			const RE::hkpCollidable* world = IsCharacter(rootA) ? rootB : rootA;
			alignas(16) float p[4], n[4];
			_mm_store_ps(p, a_point.contact.position.quad);
			_mm_store_ps(n, a_point.contact.separatingNormal.quad);
			const float sign = world == rootB ? 1.0f : -1.0f;  // the normal points out of B
			const float k = BlocksPerHavok();
			// Havok (x, y, z up) -> Minecraft (x, y up, z = -y), nudged into the surface.
			const float mx = (p[0] - n[0] * sign * kIntoSurface) * k;
			const float my = (p[2] - n[2] * sign * kIntoSurface) * k;
			const float mz = -(p[1] - n[1] * sign * kIntoSurface) * k;
			if (!IsDug(std::int32_t(std::floor(mx)), std::int32_t(std::floor(my)), std::int32_t(std::floor(mz)))) {
				return false;
			}
			return IsDiggableCollidable(world);
		}

		std::atomic<int> loggedDropped{ 0 };
		std::atomic<int> loggedPlanes{ 0 };
		std::atomic<const RE::hkpCollidable*> puppet{ nullptr };
		std::atomic<float>                    puppetFeet{ 0.0f };
		constexpr float                       kFeetSlack = 0.3f;  // Havok units above the feet still "under" them

		// The player's body while Minecraft drives it: a contact above its feet (the ground over a
		// hole or tunnel it's in, a wall at its head) would only push it away from where Minecraft
		// put it, so Skyrim would think it moved the player and pull Minecraft along.
		bool AboveThePuppetsFeet(const RE::hkpCdPoint& a_point)
		{
			const auto* body = puppet.load(std::memory_order_relaxed);
			if (!body || (Root(a_point.cdBodyA) != body && Root(a_point.cdBodyB) != body)) {
				return false;
			}
			alignas(16) float p[4];
			_mm_store_ps(p, a_point.contact.position.quad);
			return p[2] > puppetFeet.load(std::memory_order_relaxed) + kFeetSlack;
		}

		struct AddCdPointHook
		{
			static void thunk(RE::bhkCharacterPointCollector* a_this, const RE::hkpCdPoint& a_point)
			{
				Perf::Scope timer(Perf::kHookCharContacts);
				if (Any() && OnDugGround(a_point)) {
					if (loggedDropped.fetch_add(1) < 3) {
						logger::info("dig: an actor's contact with dug-away ground dropped");
					}
					return;
				}
				if (AboveThePuppetsFeet(a_point)) {
					return;
				}
				func(a_this, a_point);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Havok's plain collector: a character's support check refreshes its contacts with one
		// (checkSupport -> 0x140B9DBF0 on 1.7.104), and so do other shape queries. Without this an
		// NPC over a hole still stands on the dug-away ground: it walks on air.
		struct AllCdPointHook
		{
			static void thunk(RE::hkpAllCdPointCollector* a_this, const RE::hkpCdPoint& a_point)
			{
				Perf::Scope timer(Perf::kHookContacts);
				if (Any() && OnDugGround(a_point)) {
					return;
				}
				func(a_this, a_point);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Plane
		{
			float n[3];
			float d;  // gap between the proxy and the block (negative: overlapping)
		};

		// Minecraft's solid blocks around a proxy, as the planes Havok would make for them. A
		// vertical capsule stands in for the proxy's shape (its box).
		void BlockPlanes(const RE::hkAabb& a_box, std::vector<Plane>& a_out)
		{
			alignas(16) float lo[4], hi[4];
			_mm_store_ps(lo, a_box.min.quad);
			_mm_store_ps(hi, a_box.max.quad);
			const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f;
			const float r = std::max(0.05f, std::min(hi[0] - lo[0], hi[1] - lo[1]) * 0.5f);
			float       zBot = lo[2] + r, zTop = hi[2] - r;
			if (zTop < zBot) {
				zBot = zTop = (lo[2] + hi[2]) * 0.5f;
			}
			const float k = BlocksPerHavok();
			// Havok box -> Minecraft blocks: x = hx, y = hz, z = -hy.
			const int x0 = int(std::floor((lo[0] - kPlaneReach) * k)), x1 = int(std::floor((hi[0] + kPlaneReach) * k));
			const int y0 = int(std::floor((lo[2] - kPlaneReach) * k)), y1 = int(std::floor((hi[2] + kPlaneReach) * k));
			const int z0 = int(std::floor(-(hi[1] + kPlaneReach) * k)), z1 = int(std::floor(-(lo[1] - kPlaneReach) * k));
			if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 512) {
				return;  // something huge (a dragon): not worth it
			}
			for (int by = y0; by <= y1; ++by) {
				for (int bz = z0; bz <= z1; ++bz) {
					for (int bx = x0; bx <= x1; ++bx) {
						if (!NpcBlocks::SolidAt(bx, by, bz)) {
							continue;
						}
						// The block in Havok space.
						const float bxl = bx / k, bxh = (bx + 1) / k;
						const float byl = -(bz + 1) / k, byh = -bz / k;
						const float bzl = by / k, bzh = (by + 1) / k;
						float d[3];
						d[0] = cx - std::clamp(cx, bxl, bxh);
						d[1] = cy - std::clamp(cy, byl, byh);
						d[2] = zTop < bzl ? zTop - bzl : (zBot > bzh ? zBot - bzh : 0.0f);
						// Faces shared with another solid block are inside the wall: no edge
						// planes at the seams, or walking over a block floor would snag.
						if (d[0] > 0 && NpcBlocks::SolidAt(bx + 1, by, bz)) d[0] = 0;
						if (d[0] < 0 && NpcBlocks::SolidAt(bx - 1, by, bz)) d[0] = 0;
						if (d[1] > 0 && NpcBlocks::SolidAt(bx, by, bz - 1)) d[1] = 0;
						if (d[1] < 0 && NpcBlocks::SolidAt(bx, by, bz + 1)) d[1] = 0;
						if (d[2] > 0 && NpcBlocks::SolidAt(bx, by + 1, bz)) d[2] = 0;
						if (d[2] < 0 && NpcBlocks::SolidAt(bx, by - 1, bz)) d[2] = 0;
						const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
						Plane plane;
						if (len > 1e-5f) {
							plane = { { d[0] / len, d[1] / len, d[2] / len }, len - r };
						} else {
							// The capsule's core is inside the block: out through the top if the feet
							// are near it, else the nearest side.
							const float up = bzh - (zBot - r);
							const float side[4] = { cx - bxl, bxh - cx, cy - byl, byh - cy };
							const float nearest = std::min({ side[0], side[1], side[2], side[3] });
							if (up <= nearest + 0.25f || (!NpcBlocks::SolidAt(bx, by + 1, bz) && up < 0.6f)) {
								plane = { { 0, 0, 1 }, -up };
							} else if (nearest == side[0]) {
								plane = { { -1, 0, 0 }, -(side[0] + r) };
							} else if (nearest == side[1]) {
								plane = { { 1, 0, 0 }, -(side[1] + r) };
							} else if (nearest == side[2]) {
								plane = { { 0, -1, 0 }, -(side[2] + r) };
							} else {
								plane = { { 0, 1, 0 }, -(side[3] + r) };
							}
						}
						if (plane.d > kPlaneReach) {
							continue;
						}
						bool duplicate = false;
						for (const auto& other : a_out) {
							if (std::fabs(other.d - plane.d) < 1e-3f && other.n[0] * plane.n[0] + other.n[1] * plane.n[1] + other.n[2] * plane.n[2] > 0.9999f) {
								duplicate = true;
								break;
							}
						}
						if (!duplicate) {
							a_out.push_back(plane);
						}
					}
				}
			}
		}

		struct ProcessConstraintsHook
		{
			static void thunk(RE::bhkCharProxyController* a_this, RE::hkpCharacterProxy* a_proxy, const RE::hkArray<RE::hkpRootCdPoint>& a_manifold, RE::hkpSimplexSolverInput& a_input)
			{
				Perf::Scope timer(Perf::kHookPlanes);
				const int before = a_input.numConstraints;
				func(a_this, a_proxy, a_manifold, a_input);
				if (!a_proxy || !a_proxy->shapePhantom || !a_input.constraints) {
					return;
				}
				// Room for our planes: what Havok set aside, minus what Skyrim's callback just used.
				const int room = a_proxy->userPlanes - (a_input.numConstraints - before);
				if (a_proxy->userPlanes < kWantUserPlanes) {
					a_proxy->userPlanes = kWantUserPlanes;  // bigger arrays from the next update on
				}
				if (room <= 0) {
					return;
				}
				const auto* phantom = a_proxy->shapePhantom;
				const auto* shape = phantom->collidable.shape;
				if (!shape) {
					return;
				}
				RE::hkAabb box;
				shape->GetAabbImpl(phantom->motionState.transform, 0.0f, box);
				static thread_local std::vector<Plane> planes;
				planes.clear();
				BlockPlanes(box, planes);
				if (&phantom->collidable == puppet.load(std::memory_order_relaxed)) {
					// The player's body: only the blocks it stands on (Minecraft does the rest).
					std::erase_if(planes, [](const Plane& a_plane) { return a_plane.n[2] < 0.7f; });
				}
				if (planes.empty()) {
					return;
				}
				std::ranges::sort(planes, {}, &Plane::d);
				const int count = std::min<int>(room, static_cast<int>(planes.size()));
				if (loggedPlanes.fetch_add(1) < 3) {
					logger::info("dig: {} Minecraft block planes for an actor (room {}, nearest {:.2f})", count, room, planes[0].d);
				}
				for (int i = 0; i < count; ++i) {
					const auto& p = planes[i];
					auto&       c = a_input.constraints[a_input.numConstraints++];
					float       w = p.d - a_proxy->keepDistance;
					float       v[3] = { 0, 0, 0 };
					if (w < 0.0f) {
						const float push = -w * a_proxy->penetrationRecoverySpeed;
						v[0] = p.n[0] * push, v[1] = p.n[1] * push, v[2] = p.n[2] * push;
						w = 0.0f;
					}
					c.plane = RE::hkVector4(p.n[0], p.n[1], p.n[2], w);
					c.velocity = RE::hkVector4(v[0], v[1], v[2], 0.0f);
					c.staticFriction = a_proxy->staticFriction;
					c.extraUpStaticFriction = a_proxy->extraUpStaticFriction;
					c.extraDownStaticFriction = a_proxy->extraDownStaticFriction;
					c.dynamicFriction = a_proxy->dynamicFriction;
					c.priority = 0;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace
	{
		// Every actor's movement update (AE 37507) asks for the land's height under it (TES
		// GetLandHeight, AE 13344) and, if that's more than 30 units above the actor, puts the actor
		// back on top of the land: Skyrim's rescue for characters that fell through the ground. In a
		// dug hole or tunnel that ground is gone (or they're meant to be under it), so it mustn't.
		struct LandRescueHook
		{
			static bool thunk(RE::TES* a_tes, const RE::NiPoint3& a_pos, float& a_height)
			{
				Perf::Scope timer(Perf::kHookLand);
				const bool found = func(a_tes, a_pos, a_height);
				if (Any() && a_height > a_pos.z) {
					const auto actor = SkyToMc(a_pos);
					const auto land = SkyToMc(RE::NiPoint3(a_pos.x, a_pos.y, a_height));
					const int  x = int(std::floor(actor.x)), z = int(std::floor(actor.z));
					if (IsDug(x, int(std::floor(land.y - 0.01)), z) || IsDug(x, int(std::floor(actor.y + 0.1)), z) || IsDug(x, int(std::floor(actor.y + 1.0)), z)) {
						a_height = a_pos.z;
					}
				}
				return found;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void SetPuppet(const RE::hkpCollidable* a_proxy, float a_feetHavokZ)
	{
		puppetFeet.store(a_feetHavokZ, std::memory_order_relaxed);
		puppet.store(a_proxy, std::memory_order_relaxed);
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> collector{ RE::VTABLE_bhkCharacterPointCollector[0] };
		AddCdPointHook::func = collector.write_vfunc(0x1, AddCdPointHook::thunk);
		REL::Relocation<std::uintptr_t> plain{ RE::VTABLE_hkpAllCdPointCollector[0] };
		AllCdPointHook::func = plain.write_vfunc(0x1, AllCdPointHook::thunk);
		REL::Relocation<std::uintptr_t> controller{ RE::VTABLE_bhkCharProxyController[0] };
		ProcessConstraintsHook::func = controller.write_vfunc(0x1, ProcessConstraintsHook::thunk);
		// The land-height call in the actor movement update (checked: a 5-byte call there).
		REL::Relocation<std::uintptr_t> site{ REL::ID(37507), 0xCEA };
		if (REL::Module::IsAE() && *reinterpret_cast<const std::uint8_t*>(site.address()) == 0xE8) {
			LandRescueHook::func = SKSE::GetTrampoline().write_call<5>(site.address(), LandRescueHook::thunk);
			logger::info("dig: NPC collision hooks installed");
		} else {
			logger::warn("dig: the land-rescue call isn't where expected; actors in dug holes may be lifted out");
		}
	}
}
