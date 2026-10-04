#include "Collision.h"
#include "Dig.h"
#include "Game.h"

#include <format>

// Cuts dug blocks out of the meshes Skyrim draws: the ground (land) and diggable objects. A cut
// mesh is a clone of the original BSTriShape with its own vertex and index buffers (the parts of
// each triangle outside every dug block, re-triangulated; new vertices interpolated from the
// triangle's corners), hung next to the original, which is hidden. Skyrim's own shaders, lighting
// and shadows draw it. Reverse-engineered on 1.7.104: BSTriShape::CreateClone (0x140EF1A60) shares
// the renderer data (BSGeometry+0x138) through the renderer's add-ref, and ~BSTriShape releases it
// the same way; triangle and vertex counts are u16s at +0x158 / +0x15A. Our renderer data keeps
// one reference of our own, so Skyrim never frees it: we do, once the clone is gone.
namespace skycraft::Dig
{
	namespace
	{
		constexpr float kScanRadiusBlocks = 96.0f;  // cut meshes this far around the player
		constexpr float kRescanSeconds = 1.0f;      // look for newly loaded meshes this often

		std::atomic<ID3D11Device*> device{ nullptr };

		// Mesh data Skyrim didn't keep on the CPU, copied back from the GPU on the render thread.
		struct Readback
		{
			void*                     vertexBuffer{ nullptr };  // what it was read from
			void*                     indexBuffer{ nullptr };
			std::vector<std::uint8_t> vertices;
			std::vector<std::uint8_t> indices;
		};
		std::mutex                                                            readbackLock;
		std::vector<std::pair<RE::NiPointer<RE::BSTriShape>, void*>>          readbackQueue;
		std::unordered_map<void*, Readback>                                   readbacks;  // by renderer data

		struct Cut
		{
			RE::NiPointer<RE::BSTriShape> original;
			RE::NiPointer<RE::BSTriShape> clone;  // null: the dug blocks miss this mesh
			RE::BSGraphics::TriShape*     ours{ nullptr };
			std::uint64_t                 signature{ 0 };
			float                         up{ 0.0f };  // dug cells reach this far above their top for it
		};
		std::unordered_map<RE::BSTriShape*, Cut>  cuts;  // by original
		std::unordered_set<RE::BSTriShape*>        clones;
		std::vector<RE::BSGraphics::TriShape*>     owned;  // our renderer data, freed once Skyrim lets go
		struct Grave
		{
			RE::NiPointer<RE::BSTriShape> clone;
			int                           frames;
		};
		std::vector<Grave> graveyard;  // detached clones, kept a few frames in case a draw still holds them
		float              scanTimer = 0.0f;
		bool               scanSoon = false;
		std::uint32_t      loggedCuts = 0;

		RE::BSGraphics::TriShape*& RendererData(RE::BSTriShape* a_geom) { return a_geom->GetGeometryRuntimeData().rendererData; }

		std::uint64_t RawDesc(const RE::BSGraphics::VertexDesc& a_desc) { return reinterpret_cast<const std::uint64_t&>(a_desc); }

		float HalfToFloat(std::uint16_t a_h)
		{
			const std::uint32_t sign = std::uint32_t(a_h & 0x8000) << 16;
			std::uint32_t       exp = (a_h >> 10) & 0x1F;
			std::uint32_t       mant = a_h & 0x3FF;
			std::uint32_t       bits;
			if (exp == 0) {
				if (mant == 0) {
					bits = sign;
				} else {
					exp = 127 - 15 + 1;
					while (!(mant & 0x400)) {
						mant <<= 1;
						--exp;
					}
					mant &= 0x3FF;
					bits = sign | (exp << 23) | (mant << 13);
				}
			} else if (exp == 31) {
				bits = sign | 0x7F800000 | (mant << 13);
			} else {
				bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
			}
			float f;
			std::memcpy(&f, &bits, 4);
			return f;
		}

		std::uint16_t FloatToHalf(float a_f)
		{
			std::uint32_t bits;
			std::memcpy(&bits, &a_f, 4);
			const std::uint32_t sign = (bits >> 16) & 0x8000;
			const std::int32_t  exp = std::int32_t((bits >> 23) & 0xFF) - 127 + 15;
			std::uint32_t       mant = bits & 0x7FFFFF;
			if (exp <= 0) {
				if (exp < -10) {
					return std::uint16_t(sign);
				}
				mant = (mant | 0x800000) >> (1 - exp);
				return std::uint16_t(sign | ((mant + 0x1000) >> 13));
			}
			if (exp >= 31) {
				return std::uint16_t(sign | 0x7C00);
			}
			const std::uint32_t h = sign | (std::uint32_t(exp) << 10) | (mant >> 13);
			return std::uint16_t(h + ((mant >> 12) & 1));  // round
		}

		// How to blend each byte range of a vertex.
		struct Span
		{
			std::uint32_t offset, bytes;
			enum Kind
			{
				kFloat,
				kHalf,
				kUnorm
			} kind;
		};

		std::vector<Span> SpansOf(const RE::BSGraphics::VertexDesc& a_desc, std::uint32_t a_stride)
		{
			using V = RE::BSGraphics::Vertex;
			std::vector<Span> spans;
			const auto        flags = a_desc.GetFlags();
			for (int attr = 0; attr < V::VA_COUNT; ++attr) {
				if (!(flags & (1u << attr))) {
					continue;
				}
				Span::Kind kind = Span::kUnorm;
				if (attr == V::VA_POSITION) {
					kind = Span::kHalf;  // or floats: decided by its size below
				} else if (attr == V::VA_TEXCOORD0 || attr == V::VA_TEXCOORD1) {
					kind = Span::kHalf;
				} else if (attr == V::VA_EYEDATA) {
					kind = Span::kFloat;
				}
				spans.push_back({ a_desc.GetAttributeOffset(V::Attribute(attr)), 0, kind });
			}
			std::ranges::sort(spans, {}, &Span::offset);
			for (std::size_t i = 0; i < spans.size(); ++i) {
				const std::uint32_t end = i + 1 < spans.size() ? spans[i + 1].offset : a_stride;
				spans[i].bytes = end > spans[i].offset ? end - spans[i].offset : 0;
				// Positions take 16 bytes as floats, 8 as halves. The land has float positions
				// without the full-precision flag, so the size is what tells.
				if (spans[i].offset == 0 && spans[i].kind == Span::kHalf && (flags & V::VF_VERTEX) && spans[i].bytes >= 12) {
					spans[i].kind = Span::kFloat;
				}
			}
			return spans;
		}

		void Blend(const std::vector<Span>& a_spans, std::uint32_t a_stride, const std::uint8_t* a_a, const std::uint8_t* a_b, const std::uint8_t* a_c, const float* a_w, std::uint8_t* a_out)
		{
			std::memcpy(a_out, a_a, a_stride);
			for (const auto& s : a_spans) {
				const std::uint8_t* src[3] = { a_a + s.offset, a_b + s.offset, a_c + s.offset };
				std::uint8_t*       dst = a_out + s.offset;
				switch (s.kind) {
				case Span::kFloat:
					for (std::uint32_t i = 0; i + 4 <= s.bytes; i += 4) {
						float v = 0.0f;
						for (int k = 0; k < 3; ++k) {
							float f;
							std::memcpy(&f, src[k] + i, 4);
							v += f * a_w[k];
						}
						std::memcpy(dst + i, &v, 4);
					}
					break;
				case Span::kHalf:
					for (std::uint32_t i = 0; i + 2 <= s.bytes; i += 2) {
						float v = 0.0f;
						for (int k = 0; k < 3; ++k) {
							std::uint16_t h;
							std::memcpy(&h, src[k] + i, 2);
							v += HalfToFloat(h) * a_w[k];
						}
						const std::uint16_t h = FloatToHalf(v);
						std::memcpy(dst + i, &h, 2);
					}
					break;
				case Span::kUnorm:
					for (std::uint32_t i = 0; i < s.bytes; ++i) {
						const float v = src[0][i] * a_w[0] + src[1][i] * a_w[1] + src[2][i] * a_w[2];
						dst[i] = std::uint8_t(std::clamp(v + 0.5f, 0.0f, 255.0f));
					}
					break;
				}
			}
		}

		// Skyrim's scene graph calls, guarded: a mesh that can't be copied or hung up is left whole
		// rather than taking the game down.
		RE::BSTriShape* GuardedClone(RE::BSTriShape* a_geom)
		{
			__try {
				return static_cast<RE::BSTriShape*>(a_geom->Clone());
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		bool GuardedAttach(RE::NiNode* a_parent, RE::BSTriShape* a_child)
		{
			__try {
				a_parent->AttachChild(a_child, true);
				RE::NiUpdateData update{};
				a_child->Update(update);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool IsPlainTriShape(RE::NiAVObject* a_obj)
		{
			auto* geom = a_obj ? a_obj->AsGeometry() : nullptr;
			if (!geom || std::strcmp(geom->GetRTTI()->GetName(), "BSTriShape") != 0) {
				return false;
			}
			return !geom->GetGeometryRuntimeData().skinInstance;
		}

		void ForEachTriShape(RE::NiAVObject* a_obj, const std::function<void(RE::BSTriShape*)>& a_fn, int a_depth = 0)
		{
			if (!a_obj || a_depth > 32) {
				return;
			}
			if (IsPlainTriShape(a_obj)) {
				a_fn(static_cast<RE::BSTriShape*>(a_obj));
				return;
			}
			if (auto* node = a_obj->AsNode()) {
				for (auto& child : node->GetChildren()) {
					ForEachTriShape(child.get(), a_fn, a_depth + 1);
				}
			}
		}

		void BoundToMc(const RE::NiBound& a_bound, float a_lo[3], float a_hi[3])
		{
			const auto  c = SkyToMc(a_bound.center);
			const float r = a_bound.radius / float(proto::kUnitsPerBlock) + 0.01f;
			a_lo[0] = float(c.x) - r, a_hi[0] = float(c.x) + r;
			a_lo[1] = float(c.y) - r, a_hi[1] = float(c.y) + r;
			a_lo[2] = float(c.z) - r, a_hi[2] = float(c.z) + r;
		}

		void Bury(RE::NiPointer<RE::BSTriShape> a_clone)
		{
			if (!a_clone) {
				return;
			}
			if (auto* parent = a_clone->parent) {
				parent->DetachChild2(a_clone.get());
			}
			clones.erase(a_clone.get());
			graveyard.push_back({ std::move(a_clone), 3 });
		}

		void RemoveCut(decltype(cuts)::iterator a_it)
		{
			auto& cut = a_it->second;
			if (cut.clone) {
				Bury(std::move(cut.clone));
				if (cut.original) {
					cut.original->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
				}
			}
			cuts.erase(a_it);
		}

		void FreeOurs(RE::BSGraphics::TriShape* a_data)
		{
			if (a_data->vertexBuffer) {
				reinterpret_cast<::ID3D11Buffer*>(a_data->vertexBuffer)->Release();
			}
			if (a_data->indexBuffer) {
				reinterpret_cast<::ID3D11Buffer*>(a_data->indexBuffer)->Release();
			}
			RE::free(a_data->rawVertexData);
			RE::free(a_data->rawIndexData);
			RE::free(a_data);
		}

		::ID3D11Buffer* MakeBuffer(::ID3D11Device* a_device, const void* a_data, std::uint32_t a_bytes, UINT a_bind)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = a_bytes;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = a_bind;
			D3D11_SUBRESOURCE_DATA init{ a_data, 0, 0 };
			::ID3D11Buffer*        buffer = nullptr;
			return SUCCEEDED(a_device->CreateBuffer(&desc, &init, &buffer)) ? buffer : nullptr;
		}

		std::atomic<int> diagnostics{ 0 };
		template <class... Args>
		void Diag(spdlog::format_string_t<Args...> a_fmt, Args&&... a_args)
		{
			if (diagnostics.fetch_add(1) < 60) {
				logger::info(a_fmt, std::forward<Args>(a_args)...);
			}
		}

		// Returns false to try again later (waiting for a read-back).
		bool Build(RE::BSTriShape* a_geom, const std::vector<Clip::Cube>& a_cubes, std::uint64_t a_signature, float a_up)
		{
			auto* dev = device.load();
			auto* rd = RendererData(a_geom);
			if (!dev || !rd || !rd->vertexBuffer || !rd->indexBuffer) {
				Diag("dig: {} not cut: device {} renderer data {}", a_geom->name.c_str(), static_cast<void*>(dev), static_cast<void*>(rd));
				return true;
			}
			using V = RE::BSGraphics::Vertex;
			const auto&         desc = rd->vertexDesc;
			const std::uint32_t stride = std::uint32_t(RawDesc(desc) & 0xF) * 4;
			const auto          flags = desc.GetFlags();
			if (stride == 0 || (flags & (V::VF_SKINNED | V::VF_INSTANCEDATA | V::VF_EYEDATA)) || !(flags & V::VF_VERTEX)) {
				Diag("dig: {} not cut: vertex format {:016X} (stride {})", a_geom->name.c_str(), RawDesc(desc), stride);
				cuts[a_geom] = Cut{ RE::NiPointer<RE::BSTriShape>(a_geom), nullptr, nullptr, a_signature, a_up };
				return true;
			}
			D3D11_BUFFER_DESC vbDesc{}, ibDesc{};
			reinterpret_cast<::ID3D11Buffer*>(rd->vertexBuffer)->GetDesc(&vbDesc);
			reinterpret_cast<::ID3D11Buffer*>(rd->indexBuffer)->GetDesc(&ibDesc);
			const auto&         counts = a_geom->GetTrishapeRuntimeData();
			const std::uint32_t vertexCount = std::min<std::uint32_t>(counts.vertexCount, vbDesc.ByteWidth / stride);
			const std::uint32_t triCount = std::min<std::uint32_t>(counts.triangleCount, ibDesc.ByteWidth / 6);

			const std::uint8_t*  vertices = rd->rawVertexData;
			const std::uint16_t* indices = rd->rawIndexData;
			if (!vertices || !indices) {
				std::scoped_lock guard(readbackLock);
				const auto       it = readbacks.find(rd);
				if (it == readbacks.end() || it->second.vertexBuffer != rd->vertexBuffer || it->second.indexBuffer != rd->indexBuffer ||
					it->second.vertices.size() < std::size_t(vertexCount) * stride || it->second.indices.size() < std::size_t(triCount) * 6) {
					if (std::ranges::none_of(readbackQueue, [&](const auto& a_req) { return a_req.second == rd; })) {
						readbackQueue.emplace_back(RE::NiPointer<RE::BSTriShape>(a_geom), rd);
						Diag("dig: {} waits for its mesh from the GPU ({} vertices x {} bytes, {} triangles; buffers {} / {} bytes; have {})", a_geom->name.c_str(), vertexCount,
							stride, triCount, vbDesc.ByteWidth, ibDesc.ByteWidth, it != readbacks.end() ? "a stale copy" : "nothing");
					}
					return false;
				}
				vertices = it->second.vertices.data();
				indices = reinterpret_cast<const std::uint16_t*>(it->second.indices.data());
			}

			const auto spans = SpansOf(desc, stride);

			// Positions in Minecraft space (floats or halves: see SpansOf).
			const bool         full = !spans.empty() && spans.front().offset == 0 && spans.front().kind == Span::kFloat;
			const auto&        xf = a_geom->world;
			std::vector<float> mc(std::size_t(vertexCount) * 3);
			for (std::uint32_t i = 0; i < vertexCount; ++i) {
				const std::uint8_t* v = vertices + std::size_t(i) * stride;
				RE::NiPoint3        local;
				if (full) {
					std::memcpy(&local.x, v, 12);
				} else {
					std::uint16_t h[3];
					std::memcpy(h, v, 6);
					local = { HalfToFloat(h[0]), HalfToFloat(h[1]), HalfToFloat(h[2]) };
				}
				const auto p = SkyToMc(xf * local);
				mc[i * 3 + 0] = float(p.x);
				mc[i * 3 + 1] = float(p.y);
				mc[i * 3 + 2] = float(p.z);
			}

			std::vector<std::uint8_t>  outVerts(vertices, vertices + std::size_t(vertexCount) * stride);
			std::vector<std::uint16_t> outIndices;
			outIndices.reserve(std::size_t(triCount) * 3);
			const auto             boxes = Clip::Merge(a_cubes, a_up);
			std::vector<Clip::Box> nearBoxes;
			std::vector<Clip::Poly> pieces;
			std::uint32_t           cutTris = 0;
			bool                    overflow = false;
			for (std::uint32_t t = 0; t < triCount && !overflow; ++t) {
				const std::uint16_t idx[3] = { indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2] };
				if (idx[0] >= vertexCount || idx[1] >= vertexCount || idx[2] >= vertexCount) {
					continue;
				}
				const float* p[3] = { &mc[idx[0] * 3], &mc[idx[1] * 3], &mc[idx[2] * 3] };
				float        lo[3], hi[3];
				for (int k = 0; k < 3; ++k) {
					lo[k] = std::min({ p[0][k], p[1][k], p[2][k] });
					hi[k] = std::max({ p[0][k], p[1][k], p[2][k] });
				}
				nearBoxes.clear();
				for (const auto& b : boxes) {
					if (hi[0] >= b.lo[0] && lo[0] <= b.hi[0] && hi[1] >= b.lo[1] && lo[1] <= b.hi[1] && hi[2] >= b.lo[2] && lo[2] <= b.hi[2]) {
						nearBoxes.push_back(b);
					}
				}
				pieces.clear();
				if (nearBoxes.empty() || !Clip::Subtract(Clip::FromTriangle(p[0], p[1], p[2]), nearBoxes, pieces)) {
					outIndices.insert(outIndices.end(), idx, idx + 3);
					continue;
				}
				++cutTris;
				for (const auto& piece : pieces) {
					std::vector<std::uint16_t> ids;
					for (const auto& v : piece) {
						int corner = -1;
						for (int k = 0; k < 3; ++k) {
							if (v.b[k] > 0.99999f) {
								corner = k;
							}
						}
						if (corner >= 0) {
							ids.push_back(idx[corner]);
							continue;
						}
						const std::size_t n = outVerts.size() / stride;
						if (n >= 0xFFFF) {
							overflow = true;
							break;
						}
						outVerts.resize(outVerts.size() + stride);
						Blend(spans, stride, vertices + std::size_t(idx[0]) * stride, vertices + std::size_t(idx[1]) * stride, vertices + std::size_t(idx[2]) * stride, v.b,
							outVerts.data() + n * stride);
						ids.push_back(std::uint16_t(n));
					}
					if (overflow) {
						break;
					}
					for (std::size_t k = 1; k + 1 < ids.size(); ++k) {
						outIndices.push_back(ids[0]);
						outIndices.push_back(ids[k]);
						outIndices.push_back(ids[k + 1]);
					}
				}
			}

			auto old = cuts.find(a_geom);
			if (overflow || outIndices.size() / 3 > 0xFFFF) {
				logger::warn("dig: {} has too many vertices once cut; left whole", a_geom->name.c_str());
				if (old != cuts.end()) {
					RemoveCut(old);
				}
				cuts[a_geom] = Cut{ RE::NiPointer<RE::BSTriShape>(a_geom), nullptr, nullptr, a_signature, a_up };
				return true;
			}
			if (cutTris == 0) {
				// The dug blocks are near this mesh but miss every triangle.
				float mlo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mhi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
				for (std::size_t i = 0; i + 2 < mc.size(); i += 3) {
					for (int k = 0; k < 3; ++k) {
						mlo[k] = std::min(mlo[k], mc[i + k]);
						mhi[k] = std::max(mhi[k], mc[i + k]);
					}
				}
				Diag("dig: {} not cut: no triangle touches the {} dug blocks (first {} {} {}); mesh spans ({:.1f} {:.1f} {:.1f})-({:.1f} {:.1f} {:.1f}), {} vertices, {} triangles, "
					 "format {:016X}, {} data",
					a_geom->name.c_str(), a_cubes.size(), a_cubes[0][0], a_cubes[0][1], a_cubes[0][2], mlo[0], mlo[1], mlo[2], mhi[0], mhi[1], mhi[2], vertexCount, triCount,
					RawDesc(desc), rd->rawVertexData ? "Skyrim's" : "read-back");
				if (old != cuts.end()) {
					RemoveCut(old);
				}
				cuts[a_geom] = Cut{ RE::NiPointer<RE::BSTriShape>(a_geom), nullptr, nullptr, a_signature, a_up };
				return true;
			}

			// Our renderer data: Skyrim's layout, our buffers, one reference held by us.
			auto* data = static_cast<RE::BSGraphics::TriShape*>(RE::malloc(sizeof(RE::BSGraphics::TriShape)));
			std::memset(data, 0, sizeof(*data));
			data->vertexDesc = desc;
			const auto vbBytes = static_cast<std::uint32_t>(outVerts.size());
			const auto ibBytes = static_cast<std::uint32_t>(outIndices.size() * 2);
			auto*      vb = MakeBuffer(dev, outVerts.data(), vbBytes, D3D11_BIND_VERTEX_BUFFER);
			auto*      ib = outIndices.empty() ? nullptr : MakeBuffer(dev, outIndices.data(), ibBytes, D3D11_BIND_INDEX_BUFFER);
			if (!vb || (!ib && !outIndices.empty())) {
				Diag("dig: {} not cut: couldn't make buffers ({} + {} bytes)", a_geom->name.c_str(), vbBytes, ibBytes);
				if (vb) {
					vb->Release();
				}
				RE::free(data);
				return true;
			}
			if (!ib) {
				// Everything was dug away: a mesh with nothing in it (Skyrim still wants buffers).
				const std::uint16_t none[3] = { 0, 0, 0 };
				ib = MakeBuffer(dev, none, 6, D3D11_BIND_INDEX_BUFFER);
			}
			data->vertexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(vb);
			data->indexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(ib);
			data->rawVertexData = static_cast<std::uint8_t*>(RE::malloc(vbBytes));
			std::memcpy(data->rawVertexData, outVerts.data(), vbBytes);
			data->rawIndexData = static_cast<std::uint16_t*>(RE::malloc(std::max<std::uint32_t>(ibBytes, 6)));
			std::memset(data->rawIndexData, 0, std::max<std::uint32_t>(ibBytes, 6));
			std::memcpy(data->rawIndexData, outIndices.data(), ibBytes);

			auto*               parent = a_geom->parent;
			const std::uint32_t refsBefore = rd->refCount;
			auto*               clone = parent ? GuardedClone(a_geom) : nullptr;
			RE::NiPointer<RE::BSTriShape> cloneRef(clone);
			if (!clone || RendererData(clone) != rd || rd->refCount != refsBefore + 1) {
				logger::warn("dig: couldn't clone {} for cutting (renderer data {} -> {}, refs {} -> {})", a_geom->name.c_str(), static_cast<void*>(rd),
					clone ? static_cast<void*>(RendererData(clone)) : nullptr, refsBefore, rd->refCount);
				FreeOurs(data);
				return true;
			}
			// The clone took a reference on the original's renderer data; the original still holds
			// its own, so this never frees it.
			InterlockedDecrement(reinterpret_cast<volatile LONG*>(&rd->refCount));
			RendererData(clone) = data;
			data->refCount = 2;  // the clone's, and ours
			clone->GetTrishapeRuntimeData().vertexCount = std::uint16_t(outVerts.size() / stride);
			clone->GetTrishapeRuntimeData().triangleCount = std::uint16_t(std::max<std::size_t>(outIndices.size() / 3, 1));
			clone->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
			owned.push_back(data);

			if (old != cuts.end()) {
				Bury(std::move(old->second.clone));
				cuts.erase(old);
			}
			if (!GuardedAttach(parent, clone)) {
				logger::warn("dig: couldn't hang the cut {} in the scene", a_geom->name.c_str());
				a_geom->GetFlags().reset(RE::NiAVObject::Flag::kHidden);  // shown whole again
				return true;
			}
			a_geom->GetFlags().set(RE::NiAVObject::Flag::kHidden);
			clones.insert(clone);
			cuts[a_geom] = Cut{ RE::NiPointer<RE::BSTriShape>(a_geom), std::move(cloneRef), data, a_signature, a_up };
			if (++loggedCuts <= 20 || loggedCuts % 100 == 0) {
				logger::info("dig: cut {} ({} of {} triangles, {} -> {} vertices, {} dug blocks, {} raw data)", a_geom->name.c_str(), cutTris, triCount, vertexCount,
					outVerts.size() / stride, a_cubes.size(), rd->rawVertexData ? "Skyrim's" : "read-back");
			}
			return true;
		}

		std::uint64_t Signature(const std::vector<Clip::Cube>& a_cubes)
		{
			std::uint64_t h = 1469598103934665603ull;
			for (const auto& c : a_cubes) {
				for (int v : c) {
					h = (h ^ std::uint32_t(v)) * 1099511628211ull;
				}
			}
			return h ^ a_cubes.size();
		}

		// Dug blocks near the player, by section (16-block cube), for quick lookups by area.
		using Buckets = std::unordered_map<std::uint64_t, std::vector<Clip::Cube>>;

		std::uint64_t BucketKey(int a_sx, int a_sy, int a_sz)
		{
			return (std::uint64_t(std::uint32_t(a_sx) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_sy) & 0x1FFFFF) << 21) | (std::uint32_t(a_sz) & 0x1FFFFF);
		}

		void Process(RE::BSTriShape* a_geom, const Buckets& a_dug, float a_up)
		{
			if (clones.contains(a_geom)) {
				return;
			}
			const auto it = cuts.find(a_geom);
			// Hidden by someone else (and not by us): leave it.
			if (it == cuts.end() && a_geom->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
				return;
			}
			float lo[3], hi[3];
			BoundToMc(a_geom->worldBound, lo, hi);
			lo[1] -= a_up;  // reaches down to dug blocks this far below it
			std::vector<Clip::Cube> cubes;
			for (int sy = int(std::floor(lo[1])) >> 4; sy <= int(std::floor(hi[1])) >> 4; ++sy) {
				for (int sz = int(std::floor(lo[2])) >> 4; sz <= int(std::floor(hi[2])) >> 4; ++sz) {
					for (int sx = int(std::floor(lo[0])) >> 4; sx <= int(std::floor(hi[0])) >> 4; ++sx) {
						const auto bucket = a_dug.find(BucketKey(sx, sy, sz));
						if (bucket == a_dug.end()) {
							continue;
						}
						for (const auto& c : bucket->second) {
							if (hi[0] >= c[0] && lo[0] <= c[0] + 1 && hi[1] >= c[1] && lo[1] <= c[1] + 1 && hi[2] >= c[2] && lo[2] <= c[2] + 1) {
								cubes.push_back(c);
							}
						}
					}
				}
			}
			std::ranges::sort(cubes);
			if (cubes.empty()) {
				if (it != cuts.end()) {
					RemoveCut(it);
				}
				return;
			}
			const auto signature = Signature(cubes);
			if (it != cuts.end() && it->second.signature == signature) {
				return;
			}
			if (!Build(a_geom, cubes, signature, a_up)) {
				scanSoon = true;  // waiting for its data from the GPU
			}
		}

		// Skyrim's land (its terrain meshes are drawn with the multi-texture landscape material).
		bool IsLand(RE::BSTriShape* a_geom)
		{
			auto* property = a_geom->GetGeometryRuntimeData().shaderProperty.get();
			auto* material = property ? property->material : nullptr;
			if (!material) {
				return false;
			}
			const auto feature = material->GetFeature();
			return feature == RE::BSShaderMaterial::Feature::kMultiTexLand || feature == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend;
		}

		// A lit, solid surface (Skyrim's lighting shader), not an effect (clouds, fog, light beams
		// use the effect shader).
		bool IsSurface(RE::BSTriShape* a_geom)
		{
			auto* property = a_geom->GetGeometryRuntimeData().shaderProperty.get();
			return property && std::strcmp(property->GetRTTI()->GetName(), "BSLightingShaderProperty") == 0;
		}

		bool HasCollision(RE::NiAVObject* a_obj, int a_depth = 0)
		{
			if (!a_obj || a_depth > 32) {
				return false;
			}
			if (a_obj->collisionObject) {
				return true;
			}
			if (auto* node = a_obj->AsNode()) {
				for (auto& child : node->GetChildren()) {
					if (HasCollision(child.get(), a_depth + 1)) {
						return true;
					}
				}
			}
			return false;
		}

		// How dug cells cut a mesh: not at all, as they are, or reaching a little above their top.
		// The last is for things lying on the ground without collision of their own (roads, path
		// and moss patches): they sit a hair above the land, in the cell above the one dug out.
		constexpr float kNotDiggable = -1.0f;
		constexpr float kOverlayReach = 0.35f;

		float Diggable(RE::BSTriShape* a_geom, std::unordered_map<RE::TESObjectREFR*, float>& a_refs)
		{
			if (IsLand(a_geom)) {
				return 0.0f;
			}
			auto* ref = a_geom->GetUserData();
			if (!ref || !IsSurface(a_geom)) {
				return kNotDiggable;
			}
			const auto it = a_refs.find(ref);
			if (it != a_refs.end()) {
				return it->second;
			}
			const float up = !IsDiggableRef(ref) ? kNotDiggable : HasCollision(ref->Get3D()) ? 0.0f : kOverlayReach;
			a_refs.emplace(ref, up);
			return up;
		}

		// What's near dug blocks but left whole, once per kind (to see what might hang over a hole).
		void NoteUncut(RE::BSTriShape* a_geom)
		{
			static std::unordered_set<std::string> seenKinds;
			if (seenKinds.size() >= 40) {
				return;
			}
			auto*       ref = a_geom->GetUserData();
			auto*       base = ref ? ref->GetBaseObject() : nullptr;
			auto*       property = a_geom->GetGeometryRuntimeData().shaderProperty.get();
			auto*       model = base ? base->As<RE::TESModel>() : nullptr;
			const char* path = model && model->GetModel() ? model->GetModel() : "";
			std::string kind = std::format("{}|{}|{}", base ? int(base->GetFormType()) : -1, property ? property->GetRTTI()->GetName() : "none", path);
			if (seenKinds.insert(kind).second) {
				logger::info("dig: left whole near dug blocks: {} (form type {}, {}, model {})", a_geom->name.c_str(), base ? int(base->GetFormType()) : -1,
					property ? property->GetRTTI()->GetName() : "no shader", path);
			}
		}

		void Scan(RE::PlayerCharacter* a_player)
		{
			const auto  here = SkyToMc(a_player->GetPosition());
			const float lo[3] = { float(here.x) - kScanRadiusBlocks, float(here.y) - kScanRadiusBlocks * 0.5f, float(here.z) - kScanRadiusBlocks };
			const float hi[3] = { float(here.x) + kScanRadiusBlocks, float(here.y) + kScanRadiusBlocks * 0.5f, float(here.z) + kScanRadiusBlocks };
			std::vector<Clip::Cube> dug;
			Collect(lo, hi, dug);
			if (dug.empty() && cuts.empty()) {
				return;
			}
			// Dug blocks by section, to reject far meshes quickly.
			Buckets buckets;
			for (const auto& c : dug) {
				buckets[BucketKey(c[0] >> 4, c[1] >> 4, c[2] >> 4)].push_back(c);
			}
			auto key = BucketKey;
			auto nearDug = [&](RE::NiAVObject* a_obj) {
				float blo[3], bhi[3];
				BoundToMc(a_obj->worldBound, blo, bhi);
				if (a_obj->worldBound.radius <= 0.0f) {
					return true;  // no bound yet: look inside
				}
				if (bhi[0] - blo[0] > 512.0f) {
					return true;  // enormous: check its parts
				}
				for (int sy = int(std::floor(blo[1])) >> 4; sy <= int(std::floor(bhi[1])) >> 4; ++sy) {
					for (int sz = int(std::floor(blo[2])) >> 4; sz <= int(std::floor(bhi[2])) >> 4; ++sz) {
						for (int sx = int(std::floor(blo[0])) >> 4; sx <= int(std::floor(bhi[0])) >> 4; ++sx) {
							if (buckets.contains(key(sx, sy, sz))) {
								return true;
							}
						}
					}
				}
				return false;
			};

			std::unordered_set<RE::BSTriShape*> seen;
			// Meshes already cut: their dug blocks may have gone (another Minecraft world).
			std::vector<RE::NiPointer<RE::BSTriShape>> cutMeshes;
			for (const auto& entry : cuts) {
				cutMeshes.push_back(entry.second.original);
			}
			for (const auto& geom : cutMeshes) {
				if (geom && seen.insert(geom.get()).second) {
					Process(geom.get(), buckets, cuts.contains(geom.get()) ? cuts[geom.get()].up : 0.0f);
				}
			}
			if (dug.empty()) {
				return;
			}

			// Everything a loaded cell draws (its land and its objects), skipping far branches. Cut
			// after the walk: hanging a cut copy next to a mesh can move its parent's child list.
			std::vector<std::pair<RE::NiPointer<RE::BSTriShape>, float>> found;
			std::unordered_map<RE::TESObjectREFR*, float> refs;
			int landSeen = 0, objectsSeen = 0;
			std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* a_obj, int a_depth) {
				if (!a_obj || a_depth > 48 || !nearDug(a_obj)) {
					return;
				}
				if (IsPlainTriShape(a_obj)) {
					auto* geom = static_cast<RE::BSTriShape*>(a_obj);
					if (seen.insert(geom).second) {
						const float up = Diggable(geom, refs);
						if (up == kNotDiggable) {
							NoteUncut(geom);
						} else {
							(IsLand(geom) ? landSeen : objectsSeen)++;
							found.emplace_back(RE::NiPointer<RE::BSTriShape>(geom), up);
						}
					}
					return;
				}
				if (auto* other = a_obj->AsGeometry()) {
					// Some other kind of mesh: not cut. Say which, once, in case it matters.
					static std::unordered_set<std::string> kinds;
					if (kinds.size() < 32 && kinds.insert(other->GetRTTI()->GetName()).second) {
						logger::info("dig: not cutting {} meshes (e.g. {})", other->GetRTTI()->GetName(), other->name.c_str());
					}
					return;
				}
				if (auto* node = a_obj->AsNode()) {
					for (auto& child : node->GetChildren()) {
						walk(child.get(), a_depth + 1);
					}
				}
			};
			auto visitCell = [&](RE::TESObjectCELL* a_cell) {
				if (!a_cell) {
					return;
				}
				auto& data = a_cell->GetRuntimeData();
				if (data.loadedData && data.loadedData->cell3D) {
					walk(data.loadedData->cell3D.get(), 0);
				}
				// The land's quads, in case they hang somewhere else.
				if (auto* land = data.cellLand; land && land->loadedData) {
					for (auto* quad : land->loadedData->mesh) {
						walk(quad, 0);
					}
				}
			};
			auto* tes = RE::TES::GetSingleton();
			if (!tes) {
				return;
			}
			// Placed objects (rocks, roads, trees...): their 3D isn't always under the cell's.
			std::vector<std::array<int, 3>> removed;
			std::vector<RE::NiPointer<RE::TESObjectREFR>> toDisable;  // after the loop: it changes the cell's list
			tes->ForEachReferenceInRange(a_player, kScanRadiusBlocks * float(proto::kUnitsPerBlock), [&](RE::TESObjectREFR* a_ref) {
				// Disabled or unloading references' 3D may be on its way out: leave it alone.
				if (!a_ref || a_ref == a_player || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Is3DLoaded()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* root = a_ref->Get3D();
				if (!root || !nearDug(root)) {
					return RE::BSContainer::ForEachResult::kContinue;  // nothing dug near it (cheap: do this first)
				}
				// A small thing whose foot is on dug ground goes whole, collision and all.
				const auto foot = SkyToMc(a_ref->GetPosition());
				const int  fx = int(std::floor(foot.x)), fz = int(std::floor(foot.z));
				if ((IsDug(fx, int(std::floor(foot.y + 0.2)), fz) || IsDug(fx, int(std::floor(foot.y - 0.5)), fz)) && IsSmallThing(a_ref)) {
					static int logged = 0;
					if (++logged <= 10) {
						logger::info("dig: removed {} (its ground was dug)", a_ref->GetDisplayFullName());
					}
					const float r = root->worldBound.radius / float(proto::kUnitsPerBlock);
					for (int dx : { -1, 1 }) {
						for (int dz : { -1, 1 }) {
							removed.push_back({ int(std::floor(foot.x + dx * r)), int(std::floor(foot.y)), int(std::floor(foot.z + dz * r)) });
						}
					}
					toDisable.emplace_back(a_ref);
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (IsDiggableRef(a_ref)) {
					walk(root, 0);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (auto& ref : toDisable) {
				if (ref && !ref->IsDisabled()) {
					ref->Disable();
				}
			}
			if (!removed.empty()) {
				Collision::Get().DigChanged(removed);  // their collision is gone: Minecraft's too
			}
			if (tes->interiorCell) {
				visitCell(tes->interiorCell);
			} else if (auto* grid = tes->gridCells) {
				for (std::uint32_t x = 0; x < grid->length; ++x) {
					for (std::uint32_t y = 0; y < grid->length; ++y) {
						visitCell(grid->GetCell(x, y));
					}
				}
			}
			// Now cut what was found (nothing is being walked any more).
			for (auto& [geom, up] : found) {
				if (geom && geom->parent) {
					Process(geom.get(), buckets, up);
				}
			}
			found.clear();
			static int logged = 0;
			if (logged < 5 && (landSeen || objectsSeen)) {
				++logged;
				logger::info("dig: scan found {} land and {} object meshes near dug blocks", landSeen, objectsSeen);
			}
		}
	}

	void UpdateMeshes(RE::PlayerCharacter* a_player, float a_delta, bool a_changed)
	{
		// Clones detached a few frames ago can go now; then our renderer data Skyrim let go of.
		for (auto& grave : graveyard) {
			--grave.frames;
		}
		std::erase_if(graveyard, [](const Grave& a_grave) { return a_grave.frames <= 0; });
		std::erase_if(owned, [](RE::BSGraphics::TriShape* a_data) {
			if (a_data->refCount > 1) {
				return false;
			}
			FreeOurs(a_data);
			return true;
		});
		// Meshes Skyrim unloaded (cell detached, reference disabled...): forget their cuts.
		for (auto it = cuts.begin(); it != cuts.end();) {
			auto& cut = it->second;
			if (!cut.original || !cut.original->parent || (cut.clone && !cut.clone->parent)) {
				if (cut.original && cut.original->parent && cut.clone) {
					cut.original->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
				}
				Bury(std::move(cut.clone));
				it = cuts.erase(it);
			} else {
				++it;
			}
		}
		if (!a_player || !device.load()) {
			return;
		}
		scanSoon |= a_changed;
		scanTimer -= a_delta;
		if ((scanSoon && scanTimer <= kRescanSeconds - 0.1f) || scanTimer <= 0.0f) {
			if (Any() || !cuts.empty()) {
				const auto start = std::chrono::steady_clock::now();
				Scan(a_player);
				const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
				static float worst = 0.0f;
				static int   slow = 0;
				if (ms > 8.0f && (ms > worst * 1.5f || ++slow % 60 == 0)) {
					worst = std::max(worst, ms);
					logger::info("dig: mesh scan took {:.1f} ms ({} cut meshes)", ms, cuts.size());
				}
			}
			scanSoon = false;
			scanTimer = kRescanSeconds;
		}
	}

	void ServiceReadbacks(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		device = a_device;
		std::vector<std::pair<RE::NiPointer<RE::BSTriShape>, void*>> work;
		{
			std::scoped_lock guard(readbackLock);
			const std::size_t n = std::min<std::size_t>(readbackQueue.size(), 4);
			work.assign(std::make_move_iterator(readbackQueue.begin()), std::make_move_iterator(readbackQueue.begin() + n));
			readbackQueue.erase(readbackQueue.begin(), readbackQueue.begin() + n);
			if (readbacks.size() > 256) {
				readbacks.clear();
			}
		}
		for (auto& [geom, rdPtr] : work) {
			auto* rd = static_cast<RE::BSGraphics::TriShape*>(rdPtr);
			if (!geom || RendererData(geom.get()) != rd || !rd->vertexBuffer || !rd->indexBuffer) {
				continue;
			}
			Readback result;
			bool     ok = true;
			for (int which = 0; which < 2 && ok; ++which) {
				auto*             src = reinterpret_cast<::ID3D11Buffer*>(which == 0 ? rd->vertexBuffer : rd->indexBuffer);
				D3D11_BUFFER_DESC desc{};
				src->GetDesc(&desc);
				desc.Usage = D3D11_USAGE_STAGING;
				desc.BindFlags = 0;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				desc.MiscFlags = 0;
				::ID3D11Buffer* staging = nullptr;
				if (FAILED(a_device->CreateBuffer(&desc, nullptr, &staging))) {
					ok = false;
					break;
				}
				a_context->CopyResource(staging, src);
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (SUCCEEDED(a_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
					auto& out = which == 0 ? result.vertices : result.indices;
					out.assign(static_cast<const std::uint8_t*>(mapped.pData), static_cast<const std::uint8_t*>(mapped.pData) + desc.ByteWidth);
					a_context->Unmap(staging, 0);
				} else {
					ok = false;
				}
				staging->Release();
			}
			Diag("dig: read {} back from the GPU: {} ({} + {} bytes)", geom->name.c_str(), ok ? "ok" : "FAILED", result.vertices.size(), result.indices.size());
			if (ok) {
				result.vertexBuffer = rd->vertexBuffer;
				result.indexBuffer = rd->indexBuffer;
				std::scoped_lock guard(readbackLock);
				readbacks[rd] = std::move(result);
			}
		}
	}
}
