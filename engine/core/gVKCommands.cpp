/*
 * gVKCommands.cpp
 *
 * Command pool and command buffers of the Vulkan backend.
 */

#include "gVKCommands.h"

#ifdef GVK_DESKTOP_GLFW

#include "gUtils.h"
#include <algorithm>
#include <thread>

bool gvkCreateCommandResources(gVKContext& ctx) {
	if(ctx.device == VK_NULL_HANDLE) {
		gLoge("gVKCommands") << "Cannot create the command resources before the device exists.";
		return false;
	}

	VkCommandPoolCreateInfo poolinfo{};
	poolinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	// Drawing commands are submitted on the graphics queue, so the pool has to
	// belong to that family.
	poolinfo.queueFamilyIndex = ctx.graphicsfamily;
	// Every frame records its command buffer again from scratch. Without this flag
	// resetting an individual buffer is not allowed and the whole pool would have
	// to be reset instead.
	poolinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	VkResult result = vkCreateCommandPool(ctx.device, &poolinfo, nullptr, &ctx.commandpool);
	if(result != VK_SUCCESS) {
		gLoge("gVKCommands") << "vkCreateCommandPool failed! VkResult: " << result;
		ctx.commandpool = VK_NULL_HANDLE;
		return false;
	}

	const unsigned int workerthreads = std::max(1u, std::min(4u,
			std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u));
	ctx.shadowworkerpools.assign(GVK_MAX_FRAMES_IN_FLIGHT,
			std::vector<VkCommandPool>(workerthreads, VK_NULL_HANDLE));
	ctx.shadowworkerbuffers.assign(GVK_MAX_FRAMES_IN_FLIGHT,
			std::vector<VkCommandBuffer>(workerthreads, VK_NULL_HANDLE));
	for(int frame = 0; frame < GVK_MAX_FRAMES_IN_FLIGHT; ++frame) {
		for(unsigned int worker = 0; worker < workerthreads; ++worker) {
			VkCommandPoolCreateInfo poolinfo{};
			poolinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolinfo.queueFamilyIndex = ctx.graphicsfamily;
			poolinfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
			if(vkCreateCommandPool(ctx.device, &poolinfo, nullptr,
					&ctx.shadowworkerpools[frame][worker]) != VK_SUCCESS) {
				gvkDestroyCommandResources(ctx);
				return false;
			}
			VkCommandBufferAllocateInfo alloc{};
			alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			alloc.commandPool = ctx.shadowworkerpools[frame][worker];
			alloc.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
			alloc.commandBufferCount = 1;
			if(vkAllocateCommandBuffers(ctx.device, &alloc,
					&ctx.shadowworkerbuffers[frame][worker]) != VK_SUCCESS) {
				gvkDestroyCommandResources(ctx);
				return false;
			}
		}
	}

	// One buffer per frame in flight, so the frame being recorded never touches the
	// buffer the GPU is still reading from.
	ctx.commandbuffers.resize(GVK_MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);

	VkCommandBufferAllocateInfo allocinfo{};
	allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocinfo.commandPool = ctx.commandpool;
	// Primary: submitted to a queue directly, as opposed to being called from
	// another command buffer.
	allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocinfo.commandBufferCount = static_cast<uint32_t>(ctx.commandbuffers.size());

	result = vkAllocateCommandBuffers(ctx.device, &allocinfo, ctx.commandbuffers.data());
	if(result != VK_SUCCESS) {
		gLoge("gVKCommands") << "vkAllocateCommandBuffers failed! VkResult: " << result;
		gvkDestroyCommandResources(ctx);
		return false;
	}

	gLogi("gVKCommands") << "Command pool created with " << ctx.commandbuffers.size()
			<< " primary command buffers and " << workerthreads
			<< " shadow workers on queue family " << ctx.graphicsfamily;
	return true;
}

void gvkDestroyCommandResources(gVKContext& ctx) {
	if(ctx.device == VK_NULL_HANDLE) return;
	for(auto& pools : ctx.shadowworkerpools)
		for(VkCommandPool pool : pools)
			if(pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx.device, pool, nullptr);
	ctx.shadowworkerpools.clear();
	ctx.shadowworkerbuffers.clear();

	// Destroying the pool frees every command buffer allocated from it, so calling
	// vkFreeCommandBuffers beforehand would be redundant.
	if(ctx.commandpool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(ctx.device, ctx.commandpool, nullptr);
		ctx.commandpool = VK_NULL_HANDLE;
	}
	ctx.commandbuffers.clear();
}

#endif /* GVK_DESKTOP_GLFW */
