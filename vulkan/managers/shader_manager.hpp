/* Copyright (c) 2017-2019 Hans-Kristian Arntzen
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

#include "shader.hpp"
#include "vulkan_common.hpp"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#ifdef GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER
#include "compiler.hpp"
#endif
#include "filesystem.hpp"
#include "hash.hpp"
#ifdef GRANITE_VULKAN_MT
#include "read_write_lock.hpp"
#endif

namespace Vulkan
{
using PrecomputedShaderCache = VulkanCache<Util::IntrusivePODWrapper<Util::Hash>>;

class ShaderManager;
class Device;
class ShaderTemplate : public Util::IntrusiveHashMapEnabled<ShaderTemplate>
{
public:
	ShaderTemplate(Device *device, const std::string &shader_path, PrecomputedShaderCache &cache, Util::Hash path_hash,
	               const std::vector<std::string> &include_directories);

	bool init();

	struct Variant : public Util::IntrusiveHashMapEnabled<Variant>
	{
		Util::Hash hash = 0;
		Util::Hash spirv_hash = 0;
		std::vector<uint32_t> spirv;
		std::vector<std::pair<std::string, int>> defines;
		unsigned instance = 0;
	};

	const Variant *register_variant(const std::vector<std::pair<std::string, int>> *defines = nullptr);
	void recompile();
	void register_dependencies(ShaderManager &manager);

	Util::Hash get_path_hash() const
	{
		return path_hash;
	}

private:
	Device *device;
	std::string path;
	PrecomputedShaderCache &cache;
	Util::Hash path_hash = 0;
#ifdef GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER
	std::unique_ptr<Granite::GLSLCompiler> compiler;
	const std::vector<std::string> &include_directories;
#endif
	VulkanCache<Variant> variants;
};

class ShaderProgram : public Util::IntrusiveHashMapEnabled<ShaderProgram>
{
public:
	ShaderProgram(Device *device, PrecomputedShaderCache &cache, ShaderTemplate *compute)
		: device(device), cache(cache)
	{
		set_stage(Vulkan::ShaderStage::Compute, compute);
	}

	ShaderProgram(Device *device, PrecomputedShaderCache &cache, ShaderTemplate *vert, ShaderTemplate *frag)
		: device(device), cache(cache)
	{
		set_stage(Vulkan::ShaderStage::Vertex, vert);
		set_stage(Vulkan::ShaderStage::Fragment, frag);
	}

	Vulkan::Program *get_program(unsigned variant);
	void set_stage(Vulkan::ShaderStage stage, ShaderTemplate *shader);
	unsigned register_variant(const std::vector<std::pair<std::string, int>> &defines);

private:
	Device *device;
	PrecomputedShaderCache &cache;

	struct Variant
	{
		const ShaderTemplate::Variant *stages[static_cast<unsigned>(Vulkan::ShaderStage::Count)] = {};
		unsigned shader_instance[static_cast<unsigned>(Vulkan::ShaderStage::Count)] = {};
		Vulkan::Program *program;
#ifdef GRANITE_VULKAN_MT
		std::unique_ptr<Util::RWSpinLock> instance_lock = std::make_unique<Util::RWSpinLock>();
#endif
	};

	ShaderTemplate *stages[static_cast<unsigned>(Vulkan::ShaderStage::Count)] = {};
	std::vector<Variant> variants;
	std::vector<Util::Hash> variant_hashes;
#ifdef GRANITE_VULKAN_MT
	Util::RWSpinLock variant_lock;
#endif
};

class ShaderManager
{
public:
	ShaderManager(Device *device)
		: device(device)
	{
	}

	bool load_shader_cache(const std::string &path);
	bool save_shader_cache(const std::string &path);

	void add_include_directory(const std::string &path);

	~ShaderManager();
	ShaderProgram *register_graphics(const std::string &vertex, const std::string &fragment);
	ShaderProgram *register_compute(const std::string &compute);

#ifdef GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER
	void register_dependency(ShaderTemplate *shader, const std::string &dependency);
	void register_dependency_nolock(ShaderTemplate *shader, const std::string &dependency);
#endif

	bool get_shader_hash_by_variant_hash(Util::Hash variant_hash, Util::Hash &shader_hash);
	void register_shader_hash_from_variant_hash(Util::Hash variant_hash, Util::Hash shader_hash);

private:
	Device *device;

	PrecomputedShaderCache shader_cache;
	VulkanCache<ShaderTemplate> shaders;
	VulkanCache<ShaderProgram> programs;
	std::vector<std::string> include_directories;

	ShaderTemplate *get_template(const std::string &source);

#ifdef GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER
	std::unordered_map<std::string, std::unordered_set<ShaderTemplate *>> dependees;
#ifdef GRANITE_VULKAN_MT
	std::mutex dependency_lock;
#endif

	struct Notify
	{
		Granite::FilesystemBackend *backend;
		Granite::FileNotifyHandle handle;
	};
	std::unordered_map<std::string, Notify> directory_watches;
	void add_directory_watch(const std::string &source);
	void recompile(const Granite::FileNotifyInfo &info);
#endif
};
}