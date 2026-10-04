#include "Dig.h"
#include "Game.h"

// Skyrim's grass (tufts, ferns, ivy, pebbles) on dug ground: gone. Grass isn't placed objects: the
// grass manager's node holds one BSMultiStreamInstanceTriShape per grass type, each with groups of
// instances drawn in one go. Read on 1.7.104 (live, read-only first): a group keeps its instance
// data on the CPU (BSGraphics::VertexBuffer::m_data) as well as in its GPU buffer; an instance is
// numShortsPerInstance (16) halves, the first three its position relative to the shape's world
// translation (Skyrim axes, z up). A tuft on dug ground is sent far underground in both copies.
namespace skycraft::Dig
{
	namespace
	{
		constexpr float kBuried = -30000.0f;  // relative z for a tuft that's gone (half-float safe)
		constexpr float kRescanSeconds = 0.5f;

		std::uint16_t Half(float a_f)
		{
			std::uint32_t bits;
			std::memcpy(&bits, &a_f, 4);
			const std::uint32_t sign = (bits >> 16) & 0x8000;
			const std::int32_t  exp = std::int32_t((bits >> 23) & 0xFF) - 127 + 15;
			const std::uint32_t mant = bits & 0x7FFFFF;
			if (exp <= 0) {
				return std::uint16_t(sign);
			}
			if (exp >= 31) {
				return std::uint16_t(sign | 0x7C00);
			}
			return std::uint16_t(sign | (std::uint32_t(exp) << 10) | (mant >> 13));
		}

		float Float(std::uint16_t a_h)
		{
			const std::uint32_t sign = std::uint32_t(a_h & 0x8000) << 16;
			const std::uint32_t exp = (a_h >> 10) & 0x1F;
			const std::uint32_t mant = a_h & 0x3FF;
			std::uint32_t       bits = exp == 0 ? sign : exp == 31 ? (sign | 0x7F800000 | (mant << 13)) : (sign | ((exp + 127 - 15) << 23) | (mant << 13));
			float f;
			std::memcpy(&f, &bits, 4);
			return f;
		}

		// Per instance group: the dug state it was last checked against.
		std::unordered_map<void*, std::uint64_t> checked;
		float                                    timer = 0.0f;
		std::uint32_t                            logged = 0;

		void Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::BSGraphics::VertexBuffer* a_vb)
		{
			auto* buffer = reinterpret_cast<::ID3D11Buffer*>(a_vb->buffer);
			if (!buffer || !a_vb->m_data) {
				return;
			}
			D3D11_BUFFER_DESC desc{};
			buffer->GetDesc(&desc);
			const UINT bytes = std::min<UINT>(desc.ByteWidth, UINT(a_vb->byteWidth));
			if (desc.Usage == D3D11_USAGE_DEFAULT) {
				D3D11_BOX box{ 0, 0, 0, bytes, 1, 1 };
				a_context->UpdateSubresource(buffer, 0, &box, a_vb->m_data, 0, 0);
			} else if (desc.Usage == D3D11_USAGE_DYNAMIC) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (SUCCEEDED(a_context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					std::memcpy(mapped.pData, a_vb->m_data, bytes);
					a_context->Unmap(buffer, 0);
				}
			} else {
				// Immutable: a new buffer with the changed data in its place.
				D3D11_SUBRESOURCE_DATA init{ a_vb->m_data, 0, 0 };
				::ID3D11Buffer*        fresh = nullptr;
				if (SUCCEEDED(a_device->CreateBuffer(&desc, &init, &fresh)) && fresh) {
					a_vb->buffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(fresh);
					buffer->Release();
				}
			}
		}
	}

	void ServiceGrass(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		static auto last = std::chrono::steady_clock::now();
		const auto  now = std::chrono::steady_clock::now();
		timer -= std::chrono::duration<float>(now - last).count();
		last = now;
		if (timer > 0.0f || !Any()) {
			return;
		}
		timer = kRescanSeconds;
		auto* manager = RE::BGSGrassManager::GetSingleton();
		auto* node = manager ? manager->grassNode.get() : nullptr;
		if (!node) {
			return;
		}
		const std::uint64_t generation = Generation();
		std::unordered_set<void*> alive;
		for (auto& child : node->GetChildren()) {
			auto* object = child.get();
			if (!object || std::strcmp(object->GetRTTI()->GetName(), "BSMultiStreamInstanceTriShape") != 0) {
				continue;
			}
			auto*       shape = static_cast<RE::BSMultiStreamInstanceTriShape*>(object);
			auto&       data = shape->GetMultiStreamTrishapeRuntimeData();
			const auto  stride = std::size_t(data.instanceSize) * 2;
			const auto& origin = shape->world.translate;
			if (stride < 6) {
				continue;
			}
			for (auto* group : data.instanceGroups) {
				if (!group || !group->vertexBuffer || !group->vertexBuffer->m_data) {
					continue;
				}
				alive.insert(group);
				auto it = checked.find(group);
				if (it != checked.end() && it->second == generation) {
					continue;
				}
				checked[group] = generation;
				// The group's box (Skyrim world coords): any dug block in it?
				const auto* box = reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(group) + 0x1C);
				const auto* half = reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(group) + 0x2C);
				const auto  lo = SkyToMc(RE::NiPoint3(box[0] - half[0], box[1] + half[1], box[2] - half[2]));
				const auto  hi = SkyToMc(RE::NiPoint3(box[0] + half[0], box[1] - half[1], box[2] + half[2]));
				const float flo[3] = { float(lo.x) - 1.0f, float(lo.y) - 1.0f, float(lo.z) - 1.0f };
				const float fhi[3] = { float(hi.x) + 1.0f, float(hi.y) + 1.0f, float(hi.z) + 1.0f };
				std::vector<Clip::Cube> dug;
				Collect(flo, fhi, dug);
				if (dug.empty()) {
					continue;
				}
				auto*         bytes = static_cast<std::uint8_t*>(group->vertexBuffer->m_data);
				const auto    count = std::min<std::size_t>(group->instanceCount, group->vertexBuffer->byteWidth / stride);
				std::uint32_t buried = 0;
				for (std::size_t i = 0; i < count; ++i) {
					auto* h = reinterpret_cast<std::uint16_t*>(bytes + i * stride);
					const float rz = Float(h[2]);
					if (rz <= kBuried + 1000.0f) {
						continue;  // already gone
					}
					const auto mc = SkyToMc(RE::NiPoint3(origin.x + Float(h[0]), origin.y + Float(h[1]), origin.z + rz));
					const int  x = int(std::floor(mc.x)), z = int(std::floor(mc.z));
					if (IsDug(x, int(std::floor(mc.y - 0.1)), z) || IsDug(x, int(std::floor(mc.y + 0.1)), z)) {
						h[2] = Half(kBuried);
						++buried;
					}
				}
				if (buried) {
					Upload(a_device, a_context, group->vertexBuffer);
					if (++logged <= 10) {
						logger::info("dig: {} tufts of {} on dug ground gone", buried, shape->name.c_str());
					}
				}
			}
		}
		// Forget groups Skyrim has let go of (grass comes and goes with distance).
		std::erase_if(checked, [&](const auto& a_entry) { return !alive.contains(a_entry.first); });
	}
}
