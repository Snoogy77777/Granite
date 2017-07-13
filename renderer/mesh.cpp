#include "mesh.hpp"
#include "shader_suite.hpp"
#include "render_context.hpp"
#include "renderer.hpp"
#include <string.h>

using namespace Util;
using namespace Vulkan;

namespace Granite
{
Hash StaticMesh::get_instance_key() const
{
	Hasher h;
	h.u64(vbo_position->get_cookie());
	h.u32(position_stride);
	h.u32(topology);
	if (vbo_attributes)
	{
		h.u64(vbo_attributes->get_cookie());
		h.u32(attribute_stride);
	}
	if (ibo)
	{
		h.u64(ibo->get_cookie());
		h.u32(ibo_offset);
		h.u32(index_type);
	}
	h.u32(count);
	h.u32(vertex_offset);
	h.u32(position_stride);
	h.u64(material->get_hash());
	for (auto &attr : attributes)
	{
		h.u32(attr.format);
		h.u32(attr.offset);
	}
	return h.get();
}

namespace RenderFunctions
{
static void mesh_set_state(CommandBuffer &cmd, const StaticMeshInfo &info)
{
	cmd.set_program(*info.program);

	if (info.alpha_test)
		cmd.set_multisample_state(false, false, true);

	cmd.set_vertex_binding(0, *info.vbo_position, 0, info.position_stride);
	if (info.vbo_attributes)
		cmd.set_vertex_binding(1, *info.vbo_attributes, 0, info.attribute_stride);

	if (info.ibo)
		cmd.set_index_buffer(*info.ibo, 0, info.index_type);

	for (unsigned i = 0; i < ecast(MeshAttribute::Count); i++)
		if (info.attributes[i].format != VK_FORMAT_UNDEFINED)
			cmd.set_vertex_attrib(i, i == 0 ? 0 : 1, info.attributes[i].format, info.attributes[i].offset);

	auto &sampler = cmd.get_device().get_stock_sampler(info.sampler);
	for (unsigned i = 0; i < ecast(Material::Textures::Count); i++)
		if (info.views[i])
			cmd.set_texture(2, i, *info.views[i], sampler);

	cmd.push_constants(&info.fragment, 0, sizeof(info.fragment));
	cmd.set_primitive_topology(info.topology);

	bool primitive_restart = info.ibo && (info.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || info.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
	cmd.set_primitive_restart(primitive_restart);
	cmd.set_cull_mode(info.two_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
}

void debug_mesh_render(CommandBuffer &cmd, const RenderInfo **infos, unsigned instances)
{
	auto *info = static_cast<const DebugMeshInfo *>(infos[0]);

	cmd.set_program(*info->program);
	cmd.push_constants(&info->MVP, 0, sizeof(info->MVP));
	cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0);
	cmd.set_vertex_attrib(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0);

	unsigned count = 0;

	for (unsigned i = 0; i < instances; i++)
		count += static_cast<const DebugMeshInfo *>(infos[i])->count;

	vec3 *pos = static_cast<vec3 *>(cmd.allocate_vertex_data(0, count * sizeof(vec3), sizeof(vec3)));
	vec4 *color = static_cast<vec4 *>(cmd.allocate_vertex_data(1, count * sizeof(vec4), sizeof(vec4)));

	count = 0;
	for (unsigned i = 0; i < instances; i++)
	{
		auto &draw = *static_cast<const DebugMeshInfo *>(infos[i]);
		memcpy(pos + count, draw.positions, draw.count * sizeof(vec3));
		memcpy(color + count, draw.colors, draw.count * sizeof(vec4));
		count += draw.count;
	}

	cmd.set_depth_bias(true);
	cmd.set_depth_bias(-1.0f, -1.0f);
	cmd.draw(count);
}

void static_mesh_render(CommandBuffer &cmd, const RenderInfo **infos, unsigned instances)
{
	auto *info = static_cast<const StaticMeshInfo *>(infos[0]);
	mesh_set_state(cmd, *info);

	unsigned to_render = 0;
	for (unsigned i = 0; i < instances; i += to_render)
	{
		to_render = min<unsigned>(StaticMeshVertex::max_instances, instances - i);

		auto *vertex_data = static_cast<StaticMeshVertex *>(cmd.allocate_constant_data(3, 0, to_render * sizeof(StaticMeshVertex)));
		for (unsigned j = 0; j < to_render; j++)
			vertex_data[j] = static_cast<const StaticMeshInfo *>(infos[i + j])->vertex;

		if (info->ibo)
			cmd.draw_indexed(info->count, to_render, info->ibo_offset, info->vertex_offset, 0);
		else
			cmd.draw(info->count, to_render, info->vertex_offset, 0);
	}
}

void skinned_mesh_render(CommandBuffer &cmd, const RenderInfo **infos, unsigned instances)
{
	auto *info = static_cast<const SkinnedMeshInfo *>(infos[0]);
	mesh_set_state(cmd, *info);

	for (unsigned i = 0; i < instances; i++)
	{
		auto *vertex_data = static_cast<StaticMeshVertex *>(cmd.allocate_constant_data(3, 0, sizeof(StaticMeshVertex)));
		auto *world_transforms = static_cast<mat4 *>(cmd.allocate_constant_data(3, 1, sizeof(mat4) * info->num_bones));
		auto *normal_transforms = static_cast<mat4 *>(cmd.allocate_constant_data(3, 2, sizeof(mat4) * info->num_bones));

		memcpy(vertex_data, &static_cast<const SkinnedMeshInfo *>(infos[i])->vertex, sizeof(StaticMeshVertex));
		memcpy(world_transforms, info->world_transforms, sizeof(mat4) * info->num_bones);
		memcpy(normal_transforms, info->normal_transforms, sizeof(mat4) * info->num_bones);

		if (info->ibo)
			cmd.draw_indexed(info->count, 1, info->ibo_offset, info->vertex_offset, 0);
		else
			cmd.draw(info->count, 1, info->vertex_offset, 0);
	}
}
}

void StaticMesh::fill_render_info(StaticMeshInfo &info, const RenderContext &context,
                                  const CachedSpatialTransformComponent *transform, RenderQueue &queue) const
{
	auto type = material->pipeline == DrawPipeline::AlphaBlend ? Queue::Transparent : Queue::Opaque;
	info.render = RenderFunctions::static_mesh_render;
	info.vbo_attributes = vbo_attributes.get();
	info.vbo_position = vbo_position.get();
	info.position_stride = position_stride;
	info.attribute_stride = attribute_stride;
	info.vertex_offset = vertex_offset;

	info.ibo = ibo.get();
	info.ibo_offset = ibo_offset;
	info.index_type = index_type;
	info.count = count;
	info.sampler = material->sampler;

	info.vertex.Normal = transform ? transform->transform->normal_transform : mat4(1.0f);
	info.vertex.Model = transform ? transform->transform->world_transform : mat4(1.0f);
	info.fragment.roughness = material->roughness;
	info.fragment.metallic = material->metallic;
	info.fragment.emissive = vec4(material->emissive, 0.0f);
	info.fragment.base_color = material->base_color;
	info.fragment.lod_bias = material->lod_bias;

	info.instance_key = get_instance_key();
	info.topology = topology;
	info.two_sided = material->two_sided;
	info.alpha_test = material->pipeline == DrawPipeline::AlphaTest;

	uint32_t attrs = 0;
	uint32_t textures = 0;

	for (unsigned i = 0; i < ecast(MeshAttribute::Count); i++)
		if (attributes[i].format != VK_FORMAT_UNDEFINED)
			attrs |= 1u << i;
	memcpy(info.attributes, attributes, sizeof(attributes));

	for (unsigned i = 0; i < ecast(Material::Textures::Count); i++)
	{
		info.views[i] = material->textures[i] ? &material->textures[i]->get_image()->get_view() : nullptr;
		if (material->textures[i])
			textures |= 1u << i;
	}

	info.program = queue.get_shader_suites()[ecast(RenderableType::Mesh)].get_program(material->pipeline, attrs, textures).get();
	Hasher h;
	h.pointer(info.program);
	auto pipe_hash = h.get();

	h.u64(material->get_hash());
	h.u32(attrs);
	h.u32(textures);
	h.u64(vbo_position->get_cookie());

	if (transform)
		info.sorting_key = RenderInfo::get_sort_key(context, type, pipe_hash, h.get(), transform->world_aabb.get_center());
	else
		info.sorting_key = RenderInfo::get_background_sort_key(type, pipe_hash, h.get());
}

void StaticMesh::get_render_info(const RenderContext &context, const CachedSpatialTransformComponent *transform, RenderQueue &queue) const
{
	auto type = material->pipeline == DrawPipeline::AlphaBlend ? Queue::Transparent : Queue::Opaque;
	auto &info = queue.emplace<StaticMeshInfo>(type);
	fill_render_info(info, context, transform, queue);
}

void SkinnedMesh::get_render_info(const RenderContext &context, const CachedSpatialTransformComponent *transform, RenderQueue &queue) const
{
	auto type = material->pipeline == DrawPipeline::AlphaBlend ? Queue::Transparent : Queue::Opaque;
	auto &info = queue.emplace<SkinnedMeshInfo>(type);
	fill_render_info(info, context, transform, queue);
	info.render = RenderFunctions::skinned_mesh_render;
	info.instance_key ^= 1;

	info.num_bones = transform->skin_transform->bone_world_transforms.size();
	info.world_transforms = static_cast<mat4 *>(queue.allocate(info.num_bones * sizeof(mat4), 64));
	info.normal_transforms = static_cast<mat4 *>(queue.allocate(info.num_bones * sizeof(mat4), 64));
	memcpy(info.world_transforms, transform->skin_transform->bone_world_transforms.data(), info.num_bones * sizeof(mat4));
	memcpy(info.normal_transforms, transform->skin_transform->bone_normal_transforms.data(), info.num_bones * sizeof(mat4));
}

void StaticMesh::reset()
{
	vbo_attributes.reset();
	vbo_position.reset();
	ibo.reset();
	material.reset();
}
}
