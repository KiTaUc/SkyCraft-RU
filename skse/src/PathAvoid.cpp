#include "Game.h"

// NPCs path around Minecraft's blocks. Skyrim builds an NPC's path request in one function (AE ID
// 41642: actor, request, goal, goal radius, avoid nodes); its avoid nodes are spheres and
// cylinders the pathfinder routes around. We hook its seven callers and add a cylinder for each
// column of solid Minecraft blocks near the NPC's route, on top of any Skyrim put there itself.
namespace skycraft
{
	namespace
	{
		// BSPathingRestrictions' avoid-node array: a BSTArray with an intrusive count
		// (BSPathingRequest::ArrayRefCounted; released by the engine at count 0, 0x20 bytes).
		struct AvoidArray
		{
			RE::BSTArray<RE::BSPathingAvoidNode> nodes;  // 00
			volatile std::uint32_t               refCount;  // 18
			std::uint32_t                        pad1C;     // 1C
		};
		static_assert(sizeof(AvoidArray) == 0x20);

		using SetupFn = void(RE::Actor*, void*, void*, float, AvoidArray*);
		SetupFn* setupPath = nullptr;

		constexpr int   kMaxNodes = 96;
		constexpr float kMargin = 6.0f;  // blocks around the start-goal box that count
		// Skyrim plans paths on several AI threads at once: nothing here is shared but these counters.
		std::atomic<int> loggedEngineArrays{ 0 };
		std::atomic<int> loggedOurs{ 0 };

		// The latest path goal of each NPC (Skyrim position), for the wall steering in NpcBlocks.
		std::mutex                               goalsLock;
		std::unordered_map<RE::FormID, RE::NiPoint3> goals;

		// A BSPathingLocation's position (it comes first), read under SEH.
		bool ReadPosition(const void* a_location, RE::NiPoint3& a_out)
		{
			__try {
				const auto* p = static_cast<const float*>(a_location);
				a_out = { p[0], p[1], p[2] };
				return std::isfinite(a_out.x) && std::isfinite(a_out.y) && std::abs(a_out.x) < 1.0e7f;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		AvoidArray* BuildAvoid(RE::Actor* a_actor, const RE::NiPoint3& a_goal, AvoidArray* a_engine)
		{
			const auto from = SkyToMc(a_actor->GetPosition());
			const auto to = SkyToMc(a_goal);
			const int  x0 = int(std::floor(std::min(from.x, to.x) - kMargin)), x1 = int(std::floor(std::max(from.x, to.x) + kMargin));
			const int  z0 = int(std::floor(std::min(from.z, to.z) - kMargin)), z1 = int(std::floor(std::max(from.z, to.z) + kMargin));
			const int  y0 = int(std::floor(std::min(from.y, to.y))) - 1, y1 = int(std::floor(std::max(from.y, to.y))) + 3;
			if ((x1 - x0) * (z1 - z0) > 160 * 160) {
				return nullptr;  // a long trip: only the blocks near the start matter much, skip
			}
			struct Column
			{
				int   x, z, yLow, yHigh;
				float dist;
			};
			std::vector<Column> columns;
			for (int x = x0; x <= x1; ++x) {
				for (int z = z0; z <= z1; ++z) {
					int low = INT_MAX, high = INT_MIN;
					for (int y = y0; y <= y1; ++y) {
						if (NpcBlocks::SolidAt(x, y, z)) {
							low = std::min(low, y);
							high = std::max(high, y);
						}
					}
					if (low != INT_MAX) {
						const double dx = x + 0.5 - from.x, dz = z + 0.5 - from.z;
						columns.push_back({ x, z, low, high, float(dx * dx + dz * dz) });
					}
				}
			}
			if (columns.empty()) {
				return nullptr;
			}
			std::ranges::sort(columns, {}, &Column::dist);
			auto* out = static_cast<AvoidArray*>(RE::malloc(sizeof(AvoidArray)));
			std::construct_at(&out->nodes);
			out->refCount = 1;
			out->pad1C = 0;
			if (a_engine) {
				for (const auto& n : a_engine->nodes) {
					out->nodes.push_back(n);
				}
			}
			const int count = std::min<int>(int(columns.size()), kMaxNodes);
			for (int i = 0; i < count; ++i) {
				const auto&             c = columns[i];
				RE::BSPathingAvoidNode node{};
				node.point1 = McToSky(c.x + 0.5, c.yLow, c.z + 0.5);
				node.point2 = McToSky(c.x + 0.5, c.yHigh + 1.0, c.z + 0.5);
				node.radius = float(0.75 * proto::kUnitsPerBlock);  // the block's corners, and a little room
				node.cost = 1000.0f;
				node.avoidNodeType = RE::BSPathingAvoidNode::AvoidNodeType::AVOID_NODE_CYLINDER;
				out->nodes.push_back(node);
			}
			if (loggedOurs.fetch_add(1) < 8) {
				logger::info("path around blocks: {} heading {:.0f} blocks away, {} block columns to avoid ({} of Skyrim's own)", a_actor->GetDisplayFullName(),
					std::sqrt((to.x - from.x) * (to.x - from.x) + (to.z - from.z) * (to.z - from.z)), count, a_engine ? a_engine->nodes.size() : 0u);
			}
			return out;
		}

		void Release(AvoidArray* a_array)
		{
			if (a_array && _InterlockedDecrement(reinterpret_cast<volatile long*>(&a_array->refCount)) == 0) {
				std::destroy_at(&a_array->nodes);
				RE::free(a_array);
			}
		}

		struct SetupPathHook
		{
			static void thunk(RE::Actor* a_actor, void** a_request, void* a_goal, float a_radius, AvoidArray* a_avoid)
			{
				// Skyrim's own avoid nodes, now and then: their kinds, sizes and costs.
				if (a_avoid && a_avoid->nodes.size() > 0 && loggedEngineArrays.fetch_add(1) < 8) {
					const auto& n = a_avoid->nodes[0];
					logger::info("Skyrim path avoid nodes for {}: {} (first: type {}, radius {:.0f}, cost {:.1f})", a_actor ? a_actor->GetDisplayFullName() : "?",
						a_avoid->nodes.size(), static_cast<int>(n.avoidNodeType.underlying()), n.radius, n.cost);
				}
				AvoidArray* ours = nullptr;
				if (a_actor && !a_actor->IsPlayerRef() && State().puppeting) {
					// The goal argument is a BSPathingLocation.
					RE::NiPoint3 goal;
					if (ReadPosition(a_goal, goal)) {
						{
							std::lock_guard lock(goalsLock);
							if (goals.size() > 512) {
								goals.clear();
							}
							goals[a_actor->GetFormID()] = goal;
						}
						ours = BuildAvoid(a_actor, goal, a_avoid);
					}
				}
				setupPath(a_actor, a_request, a_goal, a_radius, ours ? ours : a_avoid);
				Release(ours);
			}
		};
	}

	namespace PathAvoid
	{
		bool GoalOf(RE::FormID a_actor, RE::NiPoint3& a_out)
		{
			std::lock_guard lock(goalsLock);
			const auto      it = goals.find(a_actor);
			if (it == goals.end()) {
				return false;
			}
			a_out = it->second;
			return true;
		}

		void Install()
		{
			if (!REL::Module::IsAE()) {
				return;
			}
			const auto target = REL::ID(41642).address();
			static constexpr std::pair<std::uint64_t, std::uintptr_t> kSites[] = { { 37778, 0xDB }, { 37819, 0x7D }, { 37820, 0xCE }, { 41608, 0x1E1 },
				{ 41610, 0xC5 }, { 41611, 0xE9 }, { 41641, 0x58 } };
			auto& trampoline = SKSE::GetTrampoline();
			int   hooked = 0;
			for (const auto& [id, offset] : kSites) {
				const auto   site = REL::ID(id).address() + offset;
				const auto*  code = reinterpret_cast<const std::uint8_t*>(site);
				std::int32_t rel = 0;
				std::memcpy(&rel, code + 1, 4);
				if (code[0] != 0xE8 || site + 5 + rel != target) {
					logger::warn("path around blocks: call site {}+{:X} doesn't match this game build; skipped", id, offset);
					continue;
				}
				trampoline.write_call<5>(site, &SetupPathHook::thunk);
				++hooked;
			}
			setupPath = reinterpret_cast<SetupFn*>(target);
			logger::info("path around blocks: hooked {} of {} NPC path setups", hooked, std::size(kSites));
		}
	}
}
