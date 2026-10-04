#include "Game.h"

// Skyrim's NPCs don't walk through Minecraft's blocks: Minecraft sends which blocks of each section
// have a collision shape (proto::kRenSolids), and every frame any NPC overlapping one is pushed
// back out sideways, so they stop at walls and slide along them.
namespace skycraft
{
	namespace
	{
		std::unordered_map<std::uint64_t, std::array<std::uint64_t, 64>> solids;  // section -> bit x + 16z + 256y
		// Written on the main thread; read there and by Skyrim's AI threads planning NPC paths.
		std::shared_mutex solidsLock;
		std::atomic<std::uint32_t> solidsGeneration{ 0 };  // bumped on every change

		std::uint64_t SectionKey(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x3FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_z) & 0x3FFFFF) << 20) |
			       (std::uint64_t(std::uint32_t(a_y) & 0xFFFFF));
		}

		int FloorDiv16(int a_v) { return a_v >> 4; }  // arithmetic shift: floor for negatives too

		constexpr float kRange = 4000.0f;          // Skyrim units from the player
		constexpr float kMaxRadiusBlocks = 0.45f;  // wider actors still fit through a one-block gap
		constexpr float kSlideSpeed = 3.2f;        // blocks a second along a wall
		// Havok keeps NPCs just clear of blocks now (DigPhysics.cpp); a wall this close still counts
		// as walked into, so they follow it round.
		constexpr double kTouchBlocks = 0.1;

		// An NPC following a wall around: which way (+1/-1 along the wall) and for how long more.
		struct Detour
		{
			int   side = 0;
			float time = 0.0f;
		};
		std::unordered_map<RE::FormID, Detour> detours;

		bool ColumnBlocked(int a_x, int a_z, int a_y0, int a_y1);
	}

	namespace NpcBlocks
	{
		bool SolidAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z);
	}

	namespace
	{
		bool ColumnBlocked(int a_x, int a_z, int a_y0, int a_y1)
		{
			for (int y = a_y0; y <= a_y1; ++y) {
				if (NpcBlocks::SolidAt(a_x, y, a_z)) {
					return true;
				}
			}
			return false;
		}
	}

	namespace NpcBlocks
	{
		void OnSolids(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenSolids)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenSolids*>(a_data);
			const auto  key = SectionKey(hdr->sx, hdr->sy, hdr->sz);
			std::unique_lock lock(solidsLock);
			++solidsGeneration;
			if (hdr->count == 0 || a_bytes < sizeof(proto::RenSolids) + 512) {
				solids.erase(key);
				return;
			}
			std::memcpy(solids[key].data(), a_data + sizeof(proto::RenSolids), 512);
		}

		void Clear()
		{
			std::unique_lock lock(solidsLock);
			++solidsGeneration;
			solids.clear();
		}

		std::uint32_t Generation() { return solidsGeneration.load(); }

		void CopySolids(const std::int32_t a_origin[3], const std::int32_t a_size[3], std::uint32_t* a_out, std::uint32_t a_bit)
		{
			std::shared_lock lock(solidsLock);
			const int x0 = a_origin[0], y0 = a_origin[1], z0 = a_origin[2];
			const int w = a_size[0], h = a_size[1], d = a_size[2];
			for (int sy = FloorDiv16(y0); sy <= FloorDiv16(y0 + h - 1); ++sy) {
				for (int sz = FloorDiv16(z0); sz <= FloorDiv16(z0 + d - 1); ++sz) {
					for (int sx = FloorDiv16(x0); sx <= FloorDiv16(x0 + w - 1); ++sx) {
						const auto it = solids.find(SectionKey(sx, sy, sz));
						if (it == solids.end()) {
							continue;
						}
						const auto& bits = it->second;
						for (int ly = 0; ly < 16; ++ly) {
							const int y = sy * 16 + ly - y0;
							if (y < 0 || y >= h) {
								continue;
							}
							for (int lz = 0; lz < 16; ++lz) {
								const int z = sz * 16 + lz - z0;
								if (z < 0 || z >= d) {
									continue;
								}
								for (int lx = 0; lx < 16; ++lx) {
									const int x = sx * 16 + lx - x0;
									const int bit = lx + 16 * lz + 256 * ly;
									if (x >= 0 && x < w && ((bits[bit >> 6] >> (bit & 63)) & 1)) {
										a_out[x + w * (y + h * z)] |= a_bit;
									}
								}
							}
						}
					}
				}
			}
		}

		bool SolidAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			std::shared_lock lock(solidsLock);
			const auto       it = solids.find(SectionKey(FloorDiv16(a_x), FloorDiv16(a_y), FloorDiv16(a_z)));
			if (it == solids.end()) {
				return false;
			}
			const int bit = (a_x & 15) + 16 * (a_z & 15) + 256 * (a_y & 15);
			return (it->second[bit >> 6] >> (bit & 63)) & 1;
		}

		// How far (blocks, up to a_max) along direction (tx, tz) from (cx, cz) until the wall on the
		// side of normal (-nx, -nz) ends, so the NPC can turn the corner there.
		double WallEnd(double cx, double cz, double tx, double tz, double nx, double nz, int y0, int y1, double a_max)
		{
			for (double s = 0.5; s <= a_max; s += 0.5) {
				const double px = cx + tx * s, pz = cz + tz * s;
				if (ColumnBlocked(int(std::floor(px)), int(std::floor(pz)), y0, y1)) {
					return a_max + 1.0;  // walled in that way too
				}
				const double wx = px - nx * 1.0, wz = pz - nz * 1.0;  // one block into the wall's side
				if (!ColumnBlocked(int(std::floor(wx)), int(std::floor(wz)), y0, y1)) {
					return s;
				}
			}
			return a_max + 1.0;
		}

		void PushActorsOut(RE::PlayerCharacter* a_player, float a_delta)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists || solids.empty()) {
				return;
			}
			const auto playerPos = a_player->GetPosition();
			for (auto& handle : lists->highActorHandles) {
				auto actorPtr = handle.get();
				auto* actor = actorPtr.get();
				if (!actor || actor == a_player || actor->IsDead() || !actor->Is3DLoaded() || actor->GetPosition().GetDistance(playerPos) > kRange) {
					continue;
				}
				const auto   sky = actor->GetPosition();
				const auto   mc = SkyToMc(sky);
				const double r = std::clamp(double(actor->GetBoundRadius()) / proto::kUnitsPerBlock, 0.2, double(kMaxRadiusBlocks)) + kTouchBlocks;
				const double h = std::clamp(double(actor->GetHeight()) / proto::kUnitsPerBlock, 0.5, 4.0);
				// From just above the feet (standing on a block is fine) to the head.
				const int y0 = int(std::floor(mc.y + 0.3)), y1 = int(std::floor(mc.y + h - 0.1));
				double    cx = mc.x, cz = mc.z;
				bool      moved = false;
				for (int pass = 0; pass < 3; ++pass) {
					double pushX = 0.0, pushZ = 0.0;
					for (int bx = int(std::floor(cx - r)); bx <= int(std::floor(cx + r)); ++bx) {
						for (int bz = int(std::floor(cz - r)); bz <= int(std::floor(cz + r)); ++bz) {
							bool hit = false;
							for (int by = y0; by <= y1 && !hit; ++by) {
								hit = SolidAt(bx, by, bz);
							}
							if (!hit) {
								continue;
							}
							// Circle (the actor) against the block's square, seen from above.
							const double qx = std::clamp(cx, double(bx), double(bx + 1)), qz = std::clamp(cz, double(bz), double(bz + 1));
							double       dx = cx - qx, dz = cz - qz;
							double       d = std::sqrt(dx * dx + dz * dz);
							if (d >= r) {
								continue;
							}
							if (d < 1e-6) {
								// Centre inside the block: out through the nearest side.
								const double toW = cx - bx, toE = bx + 1 - cx, toN = cz - bz, toS = bz + 1 - cz;
								const double m = std::min({ toW, toE, toN, toS });
								dx = m == toW ? -1.0 : m == toE ? 1.0 : 0.0;
								dz = m == toN ? -1.0 : m == toS ? 1.0 : 0.0;
								pushX += dx * (m + r);
								pushZ += dz * (m + r);
							} else {
								pushX += dx / d * (r - d);
								pushZ += dz / d * (r - d);
							}
						}
					}
					if (std::abs(pushX) < 1e-4 && std::abs(pushZ) < 1e-4) {
						break;
					}
					cx += pushX;
					cz += pushZ;
					moved = true;
				}
				auto& detour = detours[actor->GetFormID()];
				detour.time -= a_delta;
				if (moved) {
					// Walking into the wall (pushed back against its heading)? Follow it around.
					const double pushX = cx - mc.x, pushZ = cz - mc.z, pushLen = std::sqrt(pushX * pushX + pushZ * pushZ);
					if (pushLen > 1e-4) {
						const double nx = pushX / pushLen, nz = pushZ / pushLen;  // out of the wall
						double       gx = 0.0, gz = 0.0;
						RE::NiPoint3 goalSky;
						if (PathAvoid::GoalOf(actor->GetFormID(), goalSky)) {
							const auto goal = SkyToMc(goalSky);
							gx = goal.x - cx, gz = goal.z - cz;
						} else {
							const float heading = actor->GetAngleZ();  // Skyrim heading -> Minecraft x, z
							gx = std::sin(heading), gz = -std::cos(heading);
						}
						const double gl = std::sqrt(gx * gx + gz * gz);
						if (gl > 1e-3 && (gx * -nx + gz * -nz) / gl > 0.2) {
							const double tx = -nz, tz = nx;  // along the wall
							if (detour.time <= 0.0f || detour.side == 0) {
								const double endA = WallEnd(cx, cz, tx, tz, nx, nz, y0, y1, 12.0);
								const double endB = WallEnd(cx, cz, -tx, -tz, nx, nz, y0, y1, 12.0);
								const double along = (gx * tx + gz * tz) / gl;  // which end is toward the goal
								detour.side = (endA - 2.0 * along) <= (endB + 2.0 * along) ? 1 : -1;
							}
							detour.time = 1.5f;
							const double step = kSlideSpeed * a_delta;
							cx += tx * detour.side * step;
							cz += tz * detour.side * step;
						}
					}
					auto pos = McToSky(cx, mc.y, cz);
					pos.z = sky.z;
					actor->SetPosition(pos, true);
				} else if (detour.time <= 0.0f) {
					detours.erase(actor->GetFormID());
				}
			}
			if (detours.size() > 256) {
				detours.clear();
			}
		}
	}
}
