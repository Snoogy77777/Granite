/* Copyright (c) 2017-2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "meshlet_export.hpp"
#include "meshoptimizer.h"
#include "enum_cast.hpp"
#include "math.hpp"
#include "filesystem.hpp"
#include "meshlet.hpp"
#include <type_traits>
#include <limits>

namespace Granite
{
namespace Meshlet
{
using namespace Vulkan::Meshlet;

struct Metadata : Header
{
	Bound bound;
	Stream streams[MaxStreams];
};

struct CombinedMesh
{
	uint32_t stream_count;
	MeshStyle mesh_style;

	std::vector<Metadata> meshlets;
};

struct Encoded
{
	std::vector<PayloadWord> payload;
	CombinedMesh mesh;
};

struct Meshlet
{
	uint32_t global_indices_offset;
	uint32_t primitive_count;
	uint32_t vertex_count;

	const unsigned char *local_indices;
	const uint32_t *attribute_remap;
};

static i16vec3 encode_vec3_to_snorm_exp(vec3 v, int scale_log2)
{
	v.x = ldexpf(v.x, scale_log2);
	v.y = ldexpf(v.y, scale_log2);
	v.z = ldexpf(v.z, scale_log2);
	v = clamp(round(v), vec3(-0x8000), vec3(0x7fff));
	return i16vec3(v);
}

static i16vec2 encode_vec2_to_snorm_exp(vec2 v, int scale_log2)
{
	v.x = ldexpf(v.x, scale_log2);
	v.y = ldexpf(v.y, scale_log2);
	v = clamp(round(v), vec2(-0x8000), vec2(0x7fff));
	return i16vec2(v);
}

static int compute_log2_scale(float max_value)
{
	// Maximum component should have range of [1, 2) since we use floor of log2, so scale with 2^14 instead of 15.
	int max_scale_log2 = int(muglm::floor(muglm::log2(max_value)));
	int scale_log2 = 14 - max_scale_log2;
	return scale_log2;
}

template <typename T>
static void adjust_quant(std::vector<T> &values, int &exp)
{
	uint32_t active_bits = 0;
	for (auto &value : values)
		for (auto &c : value.data)
			active_bits |= c;

	if (active_bits == 0)
		return;

	int extra_shift = trailing_zeroes(active_bits);
	for (auto &value : values)
		for (auto &c : value.data)
			c >>= extra_shift;

	exp += extra_shift;
}

static std::vector<i16vec3> mesh_extract_position_snorm_exp(const SceneFormats::Mesh &mesh, int &exp)
{
	std::vector<i16vec3> encoded_positions;
	std::vector<vec3> positions;

	size_t num_positions = mesh.positions.size() / mesh.position_stride;
	positions.resize(num_positions);
	auto &layout = mesh.attribute_layout[Util::ecast(MeshAttribute::Position)];
	auto fmt = layout.format;

	if (fmt == VK_FORMAT_R32G32B32A32_SFLOAT || fmt == VK_FORMAT_R32G32B32_SFLOAT)
	{
		for (size_t i = 0; i < num_positions; i++)
		{
			memcpy(positions[i].data,
				   mesh.positions.data() + i * mesh.position_stride + layout.offset,
			       sizeof(float) * 3);
		}
	}
	else if (fmt == VK_FORMAT_UNDEFINED)
		return {};
	else
	{
		LOGE("Unexpected format %u.\n", fmt);
		return {};
	}

	vec3 max_extent = vec3(0.0f);
	for (auto &p : positions)
		max_extent = max(max_extent, abs(p));

	float max_value = max(max(max_extent.x, max_extent.y), max_extent.z);
	int log2_scale = compute_log2_scale(max_value);

	log2_scale = std::min(log2_scale, 12);

	encoded_positions.reserve(positions.size());
	for (auto &pos : positions)
		encoded_positions.push_back(encode_vec3_to_snorm_exp(pos, log2_scale));

	exp = -log2_scale;
	adjust_quant(encoded_positions, exp);

	return encoded_positions;
}

struct NormalTangent
{
	i8vec2 n;
	i8vec2 t;
	bool t_sign;
};

static std::vector<NormalTangent> mesh_extract_normal_tangent_oct8(const SceneFormats::Mesh &mesh)
{
	std::vector<NormalTangent> encoded_attributes;
	std::vector<vec4> normals;
	std::vector<vec4> tangents;

	auto &normal = mesh.attribute_layout[Util::ecast(MeshAttribute::Normal)];
	auto &tangent = mesh.attribute_layout[Util::ecast(MeshAttribute::Tangent)];

	size_t num_attrs = mesh.attributes.size() / mesh.attribute_stride;
	normals.resize(num_attrs);
	tangents.resize(num_attrs);

	if (normal.format == VK_FORMAT_R32G32B32_SFLOAT || normal.format == VK_FORMAT_R32G32B32A32_SFLOAT)
	{
		for (size_t i = 0; i < num_attrs; i++)
		{
			memcpy(normals[i].data,
			       mesh.attributes.data() + i * mesh.attribute_stride + normal.offset,
			       sizeof(float) * 3);
		}
	}
	else if (normal.format == VK_FORMAT_UNDEFINED)
	{
		for (auto &n : normals)
			n = {};
	}
	else
	{
		LOGE("Unexpected format %u.\n", normal.format);
		return {};
	}

	if (tangent.format == VK_FORMAT_R32G32B32_SFLOAT)
	{
		for (size_t i = 0; i < num_attrs; i++)
		{
			memcpy(tangents[i].data,
			       mesh.attributes.data() + i * mesh.attribute_stride + tangent.offset,
			       sizeof(float) * 3);
			tangents[i].w = 0.0f;
		}
	}
	else if (tangent.format == VK_FORMAT_R32G32B32A32_SFLOAT)
	{
		for (size_t i = 0; i < num_attrs; i++)
		{
			memcpy(tangents[i].data,
			       mesh.attributes.data() + i * mesh.attribute_stride + tangent.offset,
			       sizeof(float) * 4);
		}
	}
	else if (tangent.format == VK_FORMAT_UNDEFINED)
	{
		for (auto &t : tangents)
			t = {};
	}
	else
	{
		LOGE("Unexpected format %u.\n", tangent.format);
		return {};
	}

	encoded_attributes.reserve(normals.size());

	std::vector<i8vec4> n(normals.size());
	std::vector<i8vec4> t(normals.size());
	meshopt_encodeFilterOct(n.data(), n.size(), sizeof(i8vec4), 8, normals[0].data);
	meshopt_encodeFilterOct(t.data(), t.size(), sizeof(i8vec4), 8, tangents[0].data);

	for (size_t i = 0, size = normals.size(); i < size; i++)
		encoded_attributes.push_back({ n[i].xy(), t[i].xy(), tangents[i].w < 0.0f });

	return encoded_attributes;
}

static std::vector<i16vec2> mesh_extract_uv_snorm_scale(const SceneFormats::Mesh &mesh, int &exp)
{
	std::vector<i16vec2> encoded_uvs;
	std::vector<vec2> uvs;

	size_t num_uvs = mesh.attributes.size() / mesh.attribute_stride;
	uvs.resize(num_uvs);
	auto &layout = mesh.attribute_layout[int(MeshAttribute::UV)];
	auto fmt = layout.format;

	if (fmt == VK_FORMAT_R32G32_SFLOAT)
	{
		for (size_t i = 0; i < num_uvs; i++)
			memcpy(uvs[i].data, mesh.attributes.data() + i * mesh.attribute_stride + layout.offset, sizeof(float) * 2);
	}
	else if (fmt == VK_FORMAT_R16G16_UNORM)
	{
		for (size_t i = 0; i < num_uvs; i++)
		{
			u16vec2 u16;
			memcpy(u16.data, mesh.attributes.data() + i * mesh.attribute_stride + layout.offset, sizeof(uint16_t) * 2);
			uvs[i] = vec2(u16) * float(1.0f / 0xffff);
		}
	}
	else if (fmt == VK_FORMAT_UNDEFINED)
	{
		for (auto &uv : uvs)
			uv = {};
	}
	else
	{
		LOGE("Unexpected format %u.\n", fmt);
		return {};
	}

	vec2 max_extent = vec2(0.0f);
	for (auto &uv : uvs)
	{
		// UVs tend to be in [0, 1] range. Readjust to use more of the available range.
		uv = 2.0f * uv - 1.0f;
		max_extent = max(max_extent, abs(uv));
	}

	float max_value = max(max_extent.x, max_extent.y);
	int log2_scale = compute_log2_scale(max_value);

	encoded_uvs.reserve(uvs.size());
	for (auto &uv : uvs)
		encoded_uvs.push_back(encode_vec2_to_snorm_exp(uv, log2_scale));

	exp = -log2_scale;
	adjust_quant(encoded_uvs, exp);

	return encoded_uvs;
}

// Analyze bits required to encode a delta.
static uint32_t compute_required_bits_unsigned(uint32_t delta)
{
	return delta == 0 ? 0 : (32 - leading_zeroes(delta));
}

static vec3 decode_snorm_exp(i16vec3 p, int exp)
{
    vec3 result;
    result.x = ldexpf(float(p.x), exp);
    result.y = ldexpf(float(p.y), exp);
    result.z = ldexpf(float(p.z), exp);
    return result;
}

template <typename T>
static void write_bits(PayloadWord *words, const T *values, unsigned component_count,
                       unsigned element_index, unsigned bit_count)
{
	unsigned bit_offset = element_index * component_count * bit_count;
	for (unsigned c = 0; c < component_count; c++)
	{
		auto value = values[c];
		for (unsigned i = 0; i < bit_count; i++, bit_offset++)
			words[bit_offset / 32] |= ((value >> i) & 1) << (bit_offset & 31);
	}
}

static void encode_index_stream(std::vector<PayloadWord> &out_payload_buffer,
                                u8vec3 *stream_buffer, unsigned count)
{
	PayloadWord p[(IBOBits * 3 * PrimitivesPerChunk + 31) / 32] = {};

	for (unsigned i = 0; i < count; i++)
	{
		u8vec3 indices = stream_buffer[i];
		assert(all(lessThan(indices, u8vec3(VerticesPerChunk))));
		write_bits(p, indices.data, 3, i, IBOBits);
	}

	out_payload_buffer.insert(out_payload_buffer.end(), p, p + (IBOBits * 3 * count + 31) / 32);
}

template <int Components, typename T>
static void encode_bitplane_16_inner(std::vector<PayloadWord> &out_payload_buffer,
                                     const T *values, unsigned encoded_bits,
                                     unsigned count)
{
	PayloadWord p[16 * Components * (VerticesPerChunk / 32)] = {};
	for (uint32_t i = 0; i < count; i++)
		write_bits(p, values[i].data, Components, i, encoded_bits);

	out_payload_buffer.insert(out_payload_buffer.end(), p, p + (encoded_bits * Components * count + 31) / 32);
}

static void encode_bitplane(std::vector<PayloadWord> &out_payload_buffer,
                            const u8vec4 *values, unsigned encoded_bits,
                            unsigned count)
{
	PayloadWord p[8 * 4 * (VerticesPerChunk / 32)] = {};
	for (uint32_t i = 0; i < count; i++)
		write_bits(p, values[i].data, 4, i, encoded_bits);

	out_payload_buffer.insert(out_payload_buffer.end(), p, p + (encoded_bits * 4 * count + 31) / 32);
}

static void encode_bitplane(std::vector<PayloadWord> &out_payload_buffer,
                            const u16vec3 *values, unsigned encoded_bits,
							unsigned count)
{
	encode_bitplane_16_inner<3>(out_payload_buffer, values, encoded_bits, count);
}

static void encode_bitplane(std::vector<PayloadWord> &out_payload_buffer,
                            const u16vec2 *values, unsigned encoded_bits,
							unsigned count)
{
	encode_bitplane_16_inner<2>(out_payload_buffer, values, encoded_bits, count);
}

template <typename T> struct to_signed_vector {};
template <typename T> struct to_components {};

template <> struct to_signed_vector<u16vec3> { using type = i16vec3; };
template <> struct to_signed_vector<u16vec2> { using type = i16vec2; };
template <> struct to_components<u16vec3> { enum { components = 3 }; };
template <> struct to_components<u16vec2> { enum { components = 2 }; };
template <> struct to_signed_vector<u8vec4> { using type = i8vec4; };
template <> struct to_components<u8vec4> { enum { components = 4 }; };

template <typename T>
static auto max_component(T value) -> std::remove_reference_t<decltype(value.data[0])>
{
	std::remove_reference_t<decltype(value.data[0])> val = 0;
	for (auto v : value.data)
		val = std::max(val, v);
	return val;
}

template <typename T>
static void encode_attribute_stream(std::vector<PayloadWord> &out_payload_buffer,
                                    Stream &stream,
                                    const T *raw_attributes,
                                    uint32_t chunk_index, const uint32_t *vbo_remap,
                                    uint32_t num_attributes)
{
	using SignedT = typename to_signed_vector<T>::type;
	using UnsignedScalar = std::remove_reference_t<decltype(T()[0])>;
	using SignedScalar = std::remove_reference_t<decltype(SignedT()[0])>;
	static_assert(sizeof(T) == 4 || sizeof(T) == 6, "Encoded type must be 32 or 48 bits.");

	T attributes[VerticesPerChunk];
	for (uint32_t i = 0; i < num_attributes; i++)
		attributes[i] = raw_attributes[vbo_remap ? vbo_remap[i] : i];
	for (uint32_t i = num_attributes; i < VerticesPerChunk; i++)
		attributes[i] = attributes[0];

	T ulo{std::numeric_limits<UnsignedScalar>::max()};
	T uhi{std::numeric_limits<UnsignedScalar>::min()};
	SignedT slo{std::numeric_limits<SignedScalar>::max()};
	SignedT shi{std::numeric_limits<SignedScalar>::min()};

	for (auto &p : attributes)
	{
		ulo = min(ulo, p);
		uhi = max(uhi, p);
		slo = min(slo, SignedT(p));
		shi = max(shi, SignedT(p));
	}

	T diff_unsigned = uhi - ulo;
	T diff_signed = T(shi) - T(slo);

	unsigned diff_max_unsigned = max_component(diff_unsigned);
	unsigned diff_max_signed = max_component(diff_signed);
	if (diff_max_signed < diff_max_unsigned)
	{
		ulo = T(slo);
		diff_max_unsigned = diff_max_signed;
	}

	constexpr unsigned bits_per_component = sizeof(UnsignedScalar) * 8;

	unsigned bits = compute_required_bits_unsigned(diff_max_unsigned);

	if (bits_per_component == 16 && to_components<T>::components == 3)
	{
		// Decode math breaks for 13, 14 and 15 bits. Force 16-bit mode.
		// Encoder can choose to quantize a bit harder, so we can hit 12-bit mode.
		if (bits < 16 && bits > 12)
			bits = 16;
	}

	write_bits(stream.u.base_value, ulo.data, to_components<T>::components, chunk_index, bits_per_component);
	stream.bits_per_chunk |= bits << (8 * chunk_index);

	for (auto &p : attributes)
		p -= ulo;

	encode_bitplane(out_payload_buffer, attributes, bits, num_attributes);
}

static void encode_mesh(Encoded &encoded,
                        const Meshlet *meshlets, size_t num_meshlets,
						const void * const *pp_data,
						const int *p_aux,
                        unsigned num_streams)
{
	encoded = {};
	auto &mesh = encoded.mesh;
	assert(num_streams > 0);
	mesh.stream_count = num_streams;

	size_t num_full_meshlets = (num_meshlets + NumChunks - 1) / NumChunks;
	mesh.meshlets.reserve(num_full_meshlets);
	uint32_t base_vertex_offset = 0;
	uint32_t total_primitives = 0;

	uint32_t stream_payload_count[MaxStreams] = {};

	for (uint32_t full_meshlet_index = 0; full_meshlet_index < num_full_meshlets; full_meshlet_index++)
	{
		Metadata out_meshlet = {};
		out_meshlet.base_vertex_offset = base_vertex_offset;

		uint32_t num_chunks = std::min<uint32_t>(num_meshlets - full_meshlet_index * NumChunks, NumChunks);
		out_meshlet.num_chunks = num_chunks;

		{
			auto &index_stream = out_meshlet.streams[int(StreamType::Primitive)];
			uint32_t num_attributes = 0;
			uint32_t num_primitives = 0;

			for (uint32_t chunk_index = 0; chunk_index < num_chunks; chunk_index++)
			{
				index_stream.offsets_in_words[chunk_index] = uint32_t(encoded.payload.size());
				auto &meshlet = meshlets[full_meshlet_index * NumChunks + chunk_index];

				u8vec3 index_stream_buffer[PrimitivesPerChunk];
				for (uint32_t i = 0; i < meshlet.primitive_count; i++)
					memcpy(index_stream_buffer[i].data, meshlet.local_indices + 3 * i, 3);
				for (uint32_t i = meshlet.primitive_count; i < PrimitivesPerChunk; i++)
					index_stream_buffer[i] = u8vec3(0);

				auto &offsets = index_stream.u.offsets[chunk_index];
				offsets.attr_offset = num_attributes;
				offsets.prim_offset = num_primitives;

				auto start_count = encoded.payload.size();
				encode_index_stream(encoded.payload, index_stream_buffer, meshlet.primitive_count);
				auto end_count = encoded.payload.size();

				stream_payload_count[int(StreamType::Primitive)] += end_count - start_count;

				num_primitives += meshlet.primitive_count;
				num_attributes += meshlet.vertex_count;
				total_primitives += meshlet.primitive_count;
			}

			for (uint32_t chunk_index = num_chunks; chunk_index <= NumChunks; chunk_index++)
			{
				auto &offsets = index_stream.u.offsets[chunk_index];
				offsets.attr_offset = num_attributes;
				offsets.prim_offset = num_primitives;
			}

			base_vertex_offset += num_attributes;
		}

		for (uint32_t stream_index = 1; stream_index < num_streams; stream_index++)
		{
			auto &stream = out_meshlet.streams[stream_index];
			stream.aux = p_aux[stream_index];

			uint32_t start_count = encoded.payload.size();
			for (uint32_t chunk_index = 0; chunk_index < num_chunks; chunk_index++)
			{
				stream.offsets_in_words[chunk_index] = uint32_t(encoded.payload.size());
				auto &meshlet = meshlets[full_meshlet_index * NumChunks + chunk_index];

				switch (StreamType(stream_index))
				{
				case StreamType::Position:
					encode_attribute_stream(encoded.payload, stream,
					                        static_cast<const u16vec3 *>(pp_data[stream_index]),
											chunk_index, meshlet.attribute_remap, meshlet.vertex_count);
					break;

				case StreamType::UV:
					encode_attribute_stream(encoded.payload, stream,
					                        static_cast<const u16vec2 *>(pp_data[stream_index]),
					                        chunk_index, meshlet.attribute_remap, meshlet.vertex_count);
					break;

				case StreamType::NormalTangentOct8:
				{
					u8vec4 nts[VerticesPerChunk]{};
					uint32_t sign_mask = 0;
					auto *nt = static_cast<const NormalTangent *>(pp_data[stream_index]);
					for (unsigned i = 0; i < meshlet.vertex_count; i++)
					{
						const auto &mapped_nt = nt[meshlet.attribute_remap[i]];
						sign_mask |= uint32_t(mapped_nt.t_sign) << i;
						nts[i] = u8vec4(u8vec2(mapped_nt.n), u8vec2(mapped_nt.t));
					}

					if (meshlet.vertex_count < VerticesPerChunk && sign_mask == (1u << meshlet.vertex_count) - 1)
						sign_mask = UINT32_MAX;

					if (sign_mask == 0)
					{
						stream.aux |= 1 << (2 * chunk_index);
					}
					else if (sign_mask == UINT32_MAX)
					{
						stream.aux |= 2 << (2 * chunk_index);
					}
					else
					{
						stream.aux |= 3 << (2 * chunk_index);
						for (unsigned i = 0; i < meshlet.vertex_count; i++)
						{
							nts[i].w &= ~1;
							nts[i].w |= (sign_mask >> i) & 1u;
						}
					}

					encode_attribute_stream(encoded.payload, stream, nts,
					                        chunk_index, nullptr, meshlet.vertex_count);
					break;
				}

				default:
					break;
				}
			}
			uint32_t end_count = encoded.payload.size();
			stream_payload_count[stream_index] += end_count - start_count;
		}

		mesh.meshlets.push_back(out_meshlet);
	}

	for (unsigned i = 0; i < MaxStreams; i++)
		if (stream_payload_count[i])
			LOGI("Stream %u: %zu bytes.\n", i, stream_payload_count[i] * sizeof(PayloadWord));
	LOGI("Total primitives: %u\n", total_primitives);
	LOGI("Total vertices: %u\n", base_vertex_offset);
	LOGI("IBO fill ratio: %.3f %%\n", 100.0 * double(total_primitives) / double(mesh.meshlets.size() * MaxElementsPrim));
	LOGI("VBO fill ratio: %.3f %%\n", 100.0 * double(base_vertex_offset) / double(mesh.meshlets.size() * MaxElementsVert));
}

static bool export_encoded_mesh(const std::string &path, const Encoded &encoded)
{
	size_t required_size = 0;

	FormatHeader header = {};

	header.style = encoded.mesh.mesh_style;
	header.stream_count = encoded.mesh.stream_count;
	header.meshlet_count = uint32_t(encoded.mesh.meshlets.size());
	header.payload_size_words = uint32_t(encoded.payload.size());

	required_size += sizeof(magic);
	required_size += sizeof(FormatHeader);

	// Per-meshlet metadata.
	required_size += encoded.mesh.meshlets.size() * sizeof(Header);

	// Bounds.
	required_size += encoded.mesh.meshlets.size() * sizeof(Bound);

	// Stream metadata.
	required_size += encoded.mesh.stream_count * encoded.mesh.meshlets.size() * sizeof(Stream);

	// Payload.
	// Need a padding word to speed up decoder.
	required_size += (encoded.payload.size() + 1) * sizeof(PayloadWord);

	auto file = GRANITE_FILESYSTEM()->open(path, FileMode::WriteOnly);
	if (!file)
		return false;

	auto mapping = file->map_write(required_size);
	if (!mapping)
		return false;

	auto *ptr = mapping->mutable_data<unsigned char>();

	memcpy(ptr, magic, sizeof(magic));
	ptr += sizeof(magic);
	memcpy(ptr, &header, sizeof(header));
	ptr += sizeof(header);

	for (uint32_t i = 0; i < header.meshlet_count; i++)
	{
		auto &gpu = static_cast<const Header &>(encoded.mesh.meshlets[i]);
		memcpy(ptr, &gpu, sizeof(gpu));
		ptr += sizeof(gpu);
	}

	for (uint32_t i = 0; i < header.meshlet_count; i++)
	{
		auto &bound = encoded.mesh.meshlets[i].bound;
		memcpy(ptr, &bound, sizeof(bound));
		ptr += sizeof(bound);
	}

	for (uint32_t i = 0; i < header.meshlet_count; i++)
	{
		for (uint32_t j = 0; j < header.stream_count; j++)
		{
			memcpy(ptr, &encoded.mesh.meshlets[i].streams[j], sizeof(Stream));
			ptr += sizeof(Stream);
		}
	}

	memcpy(ptr, encoded.payload.data(), encoded.payload.size() * sizeof(PayloadWord));
	ptr += encoded.payload.size() * sizeof(PayloadWord);
	memset(ptr, 0, sizeof(PayloadWord));
	return true;
}

bool export_mesh_to_meshlet(const std::string &path, SceneFormats::Mesh mesh, MeshStyle style)
{
	mesh_deduplicate_vertices(mesh);
	if (!mesh_optimize_index_buffer(mesh, {}))
		return false;

	std::vector<i16vec3> positions;
	std::vector<i16vec2> uv;
	std::vector<NormalTangent> normal_tangent;

	unsigned num_attribute_streams = 0;
	int aux[MaxStreams] = {};
	const void *p_data[MaxStreams] = {};

	switch (style)
	{
	case MeshStyle::Skinned:
		LOGE("Unimplemented.\n");
		return false;
	case MeshStyle::Textured:
		uv = mesh_extract_uv_snorm_scale(mesh, aux[int(StreamType::UV)]);
		num_attribute_streams += 2;
		if (uv.empty())
		{
			LOGE("No UVs.\n");
			return false;
		}
		normal_tangent = mesh_extract_normal_tangent_oct8(mesh);
		if (normal_tangent.empty())
		{
			LOGE("No tangent or normal.\n");
			return false;
		}
		p_data[int(StreamType::UV)] = uv.data();
		p_data[int(StreamType::NormalTangentOct8)] = normal_tangent.data();
		// Fallthrough
	case MeshStyle::Wireframe:
		positions = mesh_extract_position_snorm_exp(mesh, aux[int(StreamType::Position)]);
		if (positions.empty())
		{
			LOGE("No positions.\n");
			return false;
		}
		p_data[int(StreamType::Position)] = positions.data();
		num_attribute_streams += 1;
		break;

	default:
		LOGE("Unknown mesh style.\n");
		return false;
	}

	// Use quantized position to guide the clustering.
	std::vector<vec3> position_buffer;
	position_buffer.reserve(positions.size());
	for (auto &p : positions)
		position_buffer.push_back(decode_snorm_exp(p, aux[int(StreamType::Position)]));

	size_t num_meshlets = meshopt_buildMeshletsBound(mesh.count, VerticesPerChunk, PrimitivesPerChunk);

	std::vector<unsigned> out_vertex_redirection_buffer(num_meshlets * VerticesPerChunk);
	std::vector<unsigned char> local_index_buffer(num_meshlets * PrimitivesPerChunk * 3);
	std::vector<meshopt_Meshlet> meshlets(num_meshlets);

	num_meshlets = meshopt_buildMeshlets(meshlets.data(),
	                                     out_vertex_redirection_buffer.data(), local_index_buffer.data(),
	                                     reinterpret_cast<const uint32_t *>(mesh.indices.data()), mesh.count,
	                                     position_buffer[0].data, positions.size(), sizeof(vec3),
	                                     VerticesPerChunk, PrimitivesPerChunk, 0.5f);

	meshlets.resize(num_meshlets);

	std::vector<Meshlet> out_meshlets;
	std::vector<uvec3> out_index_buffer;

	out_meshlets.reserve(num_meshlets);

	for (auto &meshlet : meshlets)
	{
		Meshlet m = {};

		auto *local_indices = local_index_buffer.data() + meshlet.triangle_offset;
		m.local_indices = local_indices;
		m.attribute_remap = out_vertex_redirection_buffer.data() + meshlet.vertex_offset;
		m.primitive_count = meshlet.triangle_count;
		m.vertex_count = meshlet.vertex_count;
		m.global_indices_offset = uint32_t(out_index_buffer.size());

		for (unsigned i = 0; i < meshlet.triangle_count; i++)
		{
			out_index_buffer.emplace_back(
					out_vertex_redirection_buffer[local_indices[3 * i + 0] + meshlet.vertex_offset],
					out_vertex_redirection_buffer[local_indices[3 * i + 1] + meshlet.vertex_offset],
					out_vertex_redirection_buffer[local_indices[3 * i + 2] + meshlet.vertex_offset]);
		}

		out_meshlets.push_back(m);
	}

	Encoded encoded;
	encode_mesh(encoded, out_meshlets.data(), out_meshlets.size(),
	            p_data, aux, num_attribute_streams + 1);
	encoded.mesh.mesh_style = style;

	// Compute bounds
	std::vector<meshopt_Bounds> bounds;
	bounds.clear();
	bounds.reserve((num_meshlets + NumChunks - 1) / NumChunks);

	// Fuse 8 32-size meshlets together to form a 256 meshlet.
	for (size_t i = 0, n = out_meshlets.size(); i < n; i += NumChunks)
	{
		size_t num_chunks = std::min<size_t>(n - i, NumChunks);
		uint32_t total_count = 0;
		uvec3 tmp_indices[MaxElementsPrim];

		for (size_t chunk = 0; chunk < num_chunks; chunk++)
		{
			auto &meshlet = out_meshlets[i + chunk];
			memcpy(tmp_indices[total_count].data,
			       out_index_buffer[meshlet.global_indices_offset].data,
			       meshlet.primitive_count * sizeof(tmp_indices[0]));
			total_count += meshlet.primitive_count;
		}

		auto bound = meshopt_computeClusterBounds(
				tmp_indices[0].data, total_count * 3,
				position_buffer[0].data, positions.size(), sizeof(vec3));
		bounds.push_back(bound);
	}

	assert(bounds.size() == encoded.mesh.meshlets.size());
	const auto *pbounds = bounds.data();
	for (auto &meshlet : encoded.mesh.meshlets)
	{
		memcpy(meshlet.bound.center, pbounds->center, sizeof(float) * 3);
		meshlet.bound.radius = pbounds->radius;
		memcpy(meshlet.bound.cone_axis_cutoff, pbounds->cone_axis, sizeof(pbounds->cone_axis));
		meshlet.bound.cone_axis_cutoff[3] = pbounds->cone_cutoff;
		pbounds++;
	}

	LOGI("Exported meshlet:\n");
	LOGI("  %zu meshlets\n", encoded.mesh.meshlets.size());
	LOGI("  %zu payload bytes\n", encoded.payload.size() * sizeof(PayloadWord));
	LOGI("  %u total indices\n", mesh.count);
	LOGI("  %zu total attributes\n", mesh.positions.size() / mesh.position_stride);

	size_t uncompressed_bytes = mesh.indices.size();
	uncompressed_bytes += mesh.positions.size();
	if (style != MeshStyle::Wireframe)
		uncompressed_bytes += mesh.attributes.size();

	LOGI("  %zu uncompressed bytes\n\n\n", uncompressed_bytes);

	return export_encoded_mesh(path, encoded);
}
}
}
