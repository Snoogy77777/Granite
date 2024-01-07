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

#pragma once

#include <stdint.h>

namespace Granite
{
class FileMapping;
}

namespace Vulkan
{
class CommandBuffer;
class Buffer;
}

namespace Vulkan
{
// MESHLET1 format.
namespace Meshlet
{
static constexpr unsigned MaxStreams = 8;
static constexpr unsigned NumChunks = 4;
static constexpr unsigned PrimitivesPerChunk = 64;
static constexpr unsigned IBOBits = 5;
static constexpr unsigned VerticesPerChunk = 1u << IBOBits;
static constexpr unsigned MaxElementsPrim = PrimitivesPerChunk * NumChunks;
static constexpr unsigned MaxElementsVert = VerticesPerChunk * NumChunks;

struct Stream
{
	union
	{
		uint32_t base_value[6];
		struct { uint16_t prim_offset; uint16_t attr_offset; } offsets[6];
	} u;
	uint32_t bits_per_chunk;
	int32_t aux;
	uint32_t offsets_in_words[4];
};
static_assert(sizeof(Stream) == 48, "Unexpected Stream size.");

struct Header
{
	uint32_t base_vertex_offset;
	uint32_t num_chunks;
};

// For GPU use
struct RuntimeHeader
{
	uint32_t stream_offset;
	uint32_t num_chunks;
};

struct RuntimeHeaderDecoded
{
	uint32_t primitive_offset;
	uint32_t vertex_offset;
};

struct Bound
{
	float center[3];
	float radius;
	float cone_axis_cutoff[4];
};

enum class StreamType
{
	Primitive = 0, // RGB8_UINT (fixed 5-bit encoding, fixed base value of 0)
	Position, // RGB16_SINT * 2^aux
	NormalTangentOct8, // Octahedron encoding in RG8, BA8 for tangent. Following uvec4 encodes 1-bit sign.
	UV, // (0.5 * (R16G16_SINT * 2^aux) + 0.5
	BoneIndices, // RGBA8_UINT
	BoneWeights, // RGBA8_UNORM
};

enum class MeshStyle : uint32_t
{
	Wireframe = 0, // Primitive + Position
	Textured, // Untextured + TangentOct8 + UV
	Skinned // Textured + Bone*
};

struct FormatHeader
{
	MeshStyle style;
	uint32_t stream_count;
	uint32_t meshlet_count;
	uint32_t payload_size_words;
};

using PayloadWord = uint32_t;

struct MeshView
{
	const FormatHeader *format_header;
	const Header *headers;
	const Bound *bounds;
	const Stream *streams;
	const PayloadWord *payload;
	uint32_t total_primitives;
	uint32_t total_vertices;
};

static const char magic[8] = { 'M', 'E', 'S', 'H', 'L', 'E', 'T', '3' };

MeshView create_mesh_view(const Granite::FileMapping &mapping);

enum DecodeModeFlagBits : uint32_t
{
	DECODE_MODE_UNROLLED_MESH = 1 << 0,
};
using DecodeModeFlags = uint32_t;

enum class RuntimeStyle
{
	MDI,
	Meshlet
};

struct DecodeInfo
{
	const Vulkan::Buffer *ibo, *streams[3], *indirect, *payload;
	DecodeModeFlags flags;
	MeshStyle target_style;
	RuntimeStyle runtime_style;

	struct
	{
		uint32_t primitive_offset;
		uint32_t vertex_offset;
		uint32_t meshlet_offset;
	} push;
};

bool decode_mesh(Vulkan::CommandBuffer &cmd, const DecodeInfo &decode_info, const MeshView &view);
}
}
