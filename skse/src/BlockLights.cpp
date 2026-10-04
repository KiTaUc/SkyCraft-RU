#include "Game.h"

// Minecraft's light-emitting blocks (torches, lava, glowstone, ...) as real Skyrim point lights,
// so they light Skyrim's own world (terrain, buildings, NPCs) the way a Skyrim torch would.
// Minecraft's blocks themselves already carry Minecraft's block light in their vertices, so the
// block shader leaves these lights out (IsOurs).
namespace skycraft
{
	namespace
	{
		struct Emitter
		{
			std::int32_t  x, y, z;  // Minecraft block coords
			std::uint8_t  level;    // 1-15
			std::uint8_t  kind;     // proto::LightKind
			std::uint8_t  hazard;   // proto::BlockHazard
			std::uint32_t rgb;
		};

		std::unordered_map<std::uint64_t, std::vector<Emitter>> bySection;
		std::unordered_map<std::uint64_t, std::uint8_t>         hazards;  // block -> proto::BlockHazard

		std::uint64_t BlockKey(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x3FFFFFF) << 38) | (std::uint64_t(std::uint32_t(a_z) & 0x3FFFFFF) << 12) |
			       (std::uint64_t(std::uint32_t(a_y) & 0xFFF));
		}

		std::uint64_t SectionKey(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x3FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_z) & 0x3FFFFF) << 20) |
			       (std::uint64_t(std::uint32_t(a_y) & 0xFFFFF));
		}

		// Nearby emitters merge into one light per cell (a lava lake would otherwise be hundreds).
		constexpr int   kCellBlocks = 3;
		constexpr int   kMaxLights = 24;
		constexpr float kRangeBlocks = 72.0f;   // emitters further than this from the player are left dark
		constexpr float kUpdateSeconds = 0.25f;

		struct Cluster
		{
			std::uint64_t cell;
			double        sx = 0, sy = 0, sz = 0, weight = 0;  // weighted Minecraft position
			float         r = 0, g = 0, b = 0;
			int           level = 0, count = 0;
			std::uint8_t  kind = proto::kLightSteady;
			float         radius = 0, score = 0;  // Skyrim units
			RE::NiPoint3  pos;
		};

		struct Slot
		{
			RE::NiPointer<RE::NiPointLight> light;
			std::uint64_t                   cell = 0;
			bool                            active = false;
			std::uint8_t                    kind = proto::kLightSteady;
			float                           baseFade = 1.0f, phase = 0.0f;
			RE::NiColor                     color;
		};
		std::array<Slot, kMaxLights> slots;
		float                        updateTimer = 0.0f;
		float                        clock = 0.0f;

		RE::ShadowSceneNode* Scene() { return RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]; }

		void Deactivate(Slot& a_slot)
		{
			if (a_slot.active && a_slot.light) {
				if (auto* ssn = Scene()) {
					ssn->RemoveLight(a_slot.light.get());
				}
			}
			a_slot.active = false;
			a_slot.cell = 0;
		}

		// A Skyrim point light the way Skyrim spawns its own dynamic lights (TESObjectLIGH::GenDynamic).
		void Activate(Slot& a_slot, const Cluster& a_c)
		{
			auto* ssn = Scene();
			if (!ssn) {
				return;
			}
			if (!a_slot.light) {
				a_slot.light.reset(RE::NiPointLight::Create());
				if (!a_slot.light) {
					return;
				}
			}
			auto* light = a_slot.light.get();
			auto& ld = light->GetLightRuntimeData();
			a_slot.color = { a_c.r, a_c.g, a_c.b };
			ld.ambient = { 0.0f, 0.0f, 0.0f };
			ld.diffuse = a_slot.color;
			ld.radius = { a_c.radius, a_c.radius, a_c.radius };
			// Brighter emitters shine harder, as well as further.
			a_slot.baseFade = 0.75f + 0.65f * (float(a_c.level) / 15.0f);
			ld.fade = a_slot.baseFade;
			light->SetLightAttenuation(a_c.radius);
			light->local.translate = a_c.pos;
			light->world.translate = a_c.pos;
			light->worldBound.center = a_c.pos;
			light->worldBound.radius = a_c.radius;
			a_slot.kind = a_c.kind;
			a_slot.phase = float(a_c.cell % 997) * 0.37f;
			a_slot.cell = a_c.cell;
			if (!a_slot.active || !ssn->GetPointLight(light)) {
				if (a_slot.active) {
					ssn->RemoveLight(light);  // Skyrim dropped it (cell change); add it again
				}
				RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
				params.dynamic = true;
				params.shadowLight = false;
				params.portalStrict = false;
				params.affectLand = true;
				params.affectWater = true;
				params.neverFades = true;
				params.fov = 1.0f;
				params.falloff = 1.0f;
				params.nearDistance = 5.0f;
				params.depthBias = 0.0f;
				params.sceneGraphIndex = 0;
				params.restrictedNode = nullptr;
				params.lensFlareData = nullptr;
				ssn->AddLight(light, params);
				a_slot.active = true;
			}
		}

		// Flames flicker, lava glows slowly; steady lights stay put.
		void Animate(Slot& a_slot)
		{
			if (!a_slot.active || !a_slot.light) {
				return;
			}
			float k = 1.0f;
			const float t = clock + a_slot.phase;
			if (a_slot.kind == proto::kLightFlame) {
				k = 1.0f + 0.07f * std::sin(t * 9.1f) + 0.05f * std::sin(t * 23.7f + 1.3f) + 0.03f * std::sin(t * 4.3f + 0.7f);
			} else if (a_slot.kind == proto::kLightLava) {
				k = 1.0f + 0.08f * std::sin(t * 1.3f) + 0.03f * std::sin(t * 3.1f + 2.0f);
			}
			a_slot.light->GetLightRuntimeData().fade = a_slot.baseFade * k;
		}

		void Rebuild(const McVec& a_player)
		{
			static std::unordered_map<std::uint64_t, Cluster> cells;
			cells.clear();
			const double range2 = double(kRangeBlocks) * kRangeBlocks;
			auto         floorDiv = [](std::int32_t a_v) { return a_v >= 0 ? a_v / kCellBlocks : (a_v - kCellBlocks + 1) / kCellBlocks; };
			for (const auto& [key, list] : bySection) {
				for (const auto& e : list) {
					const double dx = e.x + 0.5 - a_player.x, dy = e.y + 0.5 - a_player.y, dz = e.z + 0.5 - a_player.z;
					if (dx * dx + dy * dy + dz * dz > range2) {
						continue;
					}
					const auto cell = SectionKey(floorDiv(e.x), floorDiv(e.y), floorDiv(e.z));
					auto&      c = cells[cell];
					c.cell = cell;
					const double w = double(e.level) * e.level;
					c.sx += (e.x + 0.5) * w;
					c.sy += (e.y + 0.5) * w;
					c.sz += (e.z + 0.5) * w;
					c.weight += w;
					c.r += float(e.rgb & 0xFF) / 255.0f * float(w);
					c.g += float((e.rgb >> 8) & 0xFF) / 255.0f * float(w);
					c.b += float((e.rgb >> 16) & 0xFF) / 255.0f * float(w);
					c.level = std::max<int>(c.level, e.level);
					c.kind = std::max<std::uint8_t>(c.kind, e.kind);
					++c.count;
				}
			}
			static std::vector<Cluster> chosen;
			chosen.clear();
			const auto playerSky = McToSky(a_player.x, a_player.y, a_player.z);
			for (auto& [cell, c] : cells) {
				const float w = float(c.weight);
				c.r /= w, c.g /= w, c.b /= w;
				// A light level reaches that many blocks in Minecraft; a cluster of many reaches a bit further.
				const float blocks = float(c.level + 1) * (1.0f + 0.12f * std::log2(float(c.count)));
				c.radius = std::min(blocks, 20.0f) * float(proto::kUnitsPerBlock) * 0.8f;
				// Emitters inside a lit cell sit a little above the blocks' centre (lava lights its surface).
				c.pos = McToSky(c.sx / c.weight, c.sy / c.weight + 0.3, c.sz / c.weight);
				c.score = c.pos.GetDistance(playerSky) - c.radius;
				chosen.push_back(c);
			}
			const auto keep = std::min<std::size_t>(chosen.size(), kMaxLights);
			std::partial_sort(chosen.begin(), chosen.begin() + keep, chosen.end(), [](const Cluster& a, const Cluster& b) { return a.score < b.score; });
			chosen.resize(keep);

			// Keep lights on the clusters they already show; hand the rest to free slots.
			std::array<bool, kMaxLights> used{};
			std::vector<const Cluster*>  pending;
			for (const auto& c : chosen) {
				bool placed = false;
				for (int i = 0; i < kMaxLights; ++i) {
					if (!used[i] && slots[i].active && slots[i].cell == c.cell) {
						Activate(slots[i], c);
						used[i] = placed = true;
						break;
					}
				}
				if (!placed) {
					pending.push_back(&c);
				}
			}
			for (auto* c : pending) {
				for (int i = 0; i < kMaxLights; ++i) {
					if (!used[i]) {
						Activate(slots[i], *c);
						used[i] = true;
						break;
					}
				}
			}
			for (int i = 0; i < kMaxLights; ++i) {
				if (!used[i]) {
					Deactivate(slots[i]);
				}
			}
		}
	}

	namespace BlockLights
	{
		void OnLights(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenLights)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenLights*>(a_data);
			const auto  key = SectionKey(hdr->sx, hdr->sy, hdr->sz);
			const auto  count = std::min<std::uint64_t>(hdr->count, (a_bytes - sizeof(proto::RenLights)) / sizeof(proto::RenLight));
			if (auto it = bySection.find(key); it != bySection.end()) {
				for (const auto& e : it->second) {
					if (e.hazard) {
						hazards.erase(BlockKey(e.x, e.y, e.z));
					}
				}
			}
			if (count == 0) {
				bySection.erase(key);
				return;
			}
			auto& list = bySection[key];
			list.clear();
			const auto* src = reinterpret_cast<const proto::RenLight*>(a_data + sizeof(proto::RenLights));
			for (std::uint64_t i = 0; i < count; ++i) {
				const auto& l = src[i];
				const auto top = std::uint8_t(l.color >> 24);
				const Emitter e{ hdr->sx * 16 + l.x, hdr->sy * 16 + l.y, hdr->sz * 16 + l.z, l.level, std::uint8_t(top & 0x0F), std::uint8_t(top >> 4), l.color & 0xFFFFFF };
				list.push_back(e);
				if (e.hazard) {
					hazards[BlockKey(e.x, e.y, e.z)] = e.hazard;
				}
			}
			updateTimer = 0.0f;  // show a new torch at once
		}

		void Clear()
		{
			bySection.clear();
			hazards.clear();
			updateTimer = 0.0f;
		}

		void Update(const McVec* a_player, float a_delta)
		{
			clock += a_delta;
			if (!a_player) {
				for (auto& slot : slots) {
					Deactivate(slot);
				}
				return;
			}
			updateTimer -= a_delta;
			if (updateTimer <= 0.0f) {
				updateTimer = kUpdateSeconds;
				Rebuild(*a_player);
			}
			for (auto& slot : slots) {
				Animate(slot);
			}
		}

		std::uint8_t HazardAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			const auto it = hazards.find(BlockKey(a_x, a_y, a_z));
			return it != hazards.end() ? it->second : std::uint8_t(proto::kHazardNone);
		}

		bool IsOurs(const RE::NiLight* a_light)
		{
			for (const auto& slot : slots) {
				if (slot.light && slot.light.get() == a_light) {
					return true;
				}
			}
			return false;
		}
	}
}
