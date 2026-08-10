/*
 * gVKShadow.cpp
 *
 * Depth-only shadow map of the Vulkan 3D path.
 */

#include "gVKShadow.h"

#ifdef GVK_DESKTOP_GLFW

#include "gVKBuffer.h"
#include "gVKRenderTarget.h"
#include "gUtils.h"

// Shadow depth is sampled comparatively rather than used for the main camera's
// precision. Prefer 16-bit UNORM here: a 4096-square D32 image is 64 MiB and has
// to be stored and sampled every frame, while D16 halves that traffic. OpenGL
// independently chooses a compact fixed-point depth texture for the same job.
// Check both uses because the generic depth chooser only guarantees attachment
// support, whereas this image is also a sampler input in the colour pass.
static VkFormat gvkFindShadowDepthFormat(gVKContext& ctx) {
	const VkPhysicalDevice physicaldevice = *ctx.getPhysicalDevice();
	if(physicaldevice == VK_NULL_HANDLE) return VK_FORMAT_UNDEFINED;
	const VkFormat candidates[] = {
			VK_FORMAT_D16_UNORM,
			VK_FORMAT_D24_UNORM_S8_UINT,
			VK_FORMAT_D32_SFLOAT};
	const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
			| VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	for(VkFormat format : candidates) {
		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(physicaldevice, format, &properties);
		if((properties.optimalTilingFeatures & required) == required) return format;
	}
	return VK_FORMAT_UNDEFINED;
}

// Creates the render pass the shadow map is drawn into: one depth attachment, no
// colour. Unlike the screen pass this one stores its result, because the second
// pass samples it.
// Takes the device and format rather than the context, so it needs no friendship
// with gVKContext - it is file-local and never named in the header.
static bool gvkCreateShadowRenderPass(VkDevice device, VkFormat depthformat, VkRenderPass& outPass) {
	VkAttachmentDescription depthattachment{};
	depthattachment.format = depthformat;
	depthattachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthattachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	// STORE, not DONT_CARE: this is the whole output of the pass.
	depthattachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	depthattachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthattachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthattachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	// Ends the pass ready to be sampled, so no separate barrier is needed before the
	// shading pass reads it.
	depthattachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkAttachmentReference depthref{};
	depthref.attachment = 0;
	depthref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 0;
	subpass.pDepthStencilAttachment = &depthref;

	// The frame's colour pass samples this image after the depth pass. There is no
	// incoming read-after-write dependency from the previous frame because each
	// frame-in-flight owns a different image and its fence protects reuse.
	VkSubpassDependency dependency{};
	dependency.srcSubpass = 0;
	dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
	dependency.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	VkRenderPassCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 1;
	info.pAttachments = &depthattachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 1;
	info.pDependencies = &dependency;

	if(vkCreateRenderPass(device, &info, nullptr, &outPass) != VK_SUCCESS) {
		gLoge("gVKShadow") << "vkCreateRenderPass failed for the shadow map.";
		outPass = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

bool gvkCreateShadowResources(gVKContext& ctx, uint32_t width, uint32_t height) {
	VkDevice device = *ctx.getDevice();
	if(device == VK_NULL_HANDLE || width == 0 || height == 0) return false;

	gvkDestroyShadowResources(ctx);

	ctx.shadowformat = gvkFindShadowDepthFormat(ctx);
	if(ctx.shadowformat == VK_FORMAT_UNDEFINED) {
		gLoge("gVKShadow") << "No supported depth format for the shadow map.";
		return false;
	}
	ctx.shadowextent = {width, height};

	ctx.shadowimages.assign(GVK_MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
	ctx.shadowmemories.assign(GVK_MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
	ctx.shadowviews.assign(GVK_MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
	ctx.shadowframebuffers.assign(GVK_MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
	ctx.shadowdescriptorsets.assign(GVK_MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);

	VkImageCreateInfo imageinfo{};
	imageinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageinfo.imageType = VK_IMAGE_TYPE_2D;
	imageinfo.extent = {width, height, 1};
	imageinfo.mipLevels = 1;
	imageinfo.arrayLayers = 1;
	imageinfo.format = ctx.shadowformat;
	imageinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	// SAMPLED as well as DEPTH_STENCIL_ATTACHMENT: written as depth, read as a
	// texture. That pair is the whole idea of a shadow map.
	imageinfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageinfo.samples = VK_SAMPLE_COUNT_1_BIT;
	VkImageViewCreateInfo viewinfo{};
	viewinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewinfo.format = ctx.shadowformat;
	viewinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewinfo.subresourceRange.baseMipLevel = 0;
	viewinfo.subresourceRange.levelCount = 1;
	viewinfo.subresourceRange.baseArrayLayer = 0;
	viewinfo.subresourceRange.layerCount = 1;
	for(int frame = 0; frame < GVK_MAX_FRAMES_IN_FLIGHT; ++frame) {
		if(vkCreateImage(device, &imageinfo, nullptr, &ctx.shadowimages[frame]) != VK_SUCCESS) {
			gLoge("gVKShadow") << "vkCreateImage failed for shadow frame " << frame << ".";
			gvkDestroyShadowResources(ctx);
			return false;
		}
		VkMemoryRequirements memreq{};
		vkGetImageMemoryRequirements(device, ctx.shadowimages[frame], &memreq);
		VkMemoryAllocateInfo allocinfo{};
		allocinfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocinfo.allocationSize = memreq.size;
		allocinfo.memoryTypeIndex = gvkFindMemoryType(ctx, memreq.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if(vkAllocateMemory(device, &allocinfo, nullptr, &ctx.shadowmemories[frame]) != VK_SUCCESS) {
			gLoge("gVKShadow") << "vkAllocateMemory failed for shadow frame " << frame << ".";
			gvkDestroyShadowResources(ctx);
			return false;
		}
		vkBindImageMemory(device, ctx.shadowimages[frame], ctx.shadowmemories[frame], 0);
		viewinfo.image = ctx.shadowimages[frame];
		if(vkCreateImageView(device, &viewinfo, nullptr, &ctx.shadowviews[frame]) != VK_SUCCESS) {
			gLoge("gVKShadow") << "vkCreateImageView failed for shadow frame " << frame << ".";
			gvkDestroyShadowResources(ctx);
			return false;
		}
	}

	VkSamplerCreateInfo samplerinfo{};
	samplerinfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	// Nearest, and the shader does its own PCF - the same arrangement as the OpenGL
	// path, which takes several taps by hand rather than leaning on the sampler.
	samplerinfo.magFilter = VK_FILTER_NEAREST;
	samplerinfo.minFilter = VK_FILTER_NEAREST;
	samplerinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	// Clamped to a white border so anything outside the light's frustum reads as
	// "furthest away", i.e. lit. Repeating there would wrap the shadow of one edge
	// of the scene onto the other.
	samplerinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerinfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	samplerinfo.minLod = 0.0f;
	samplerinfo.maxLod = 0.0f;
	if(vkCreateSampler(device, &samplerinfo, nullptr, &ctx.shadowsampler) != VK_SUCCESS) {
		gLoge("gVKShadow") << "vkCreateSampler failed for the shadow map.";
		gvkDestroyShadowResources(ctx);
		return false;
	}

	if(!gvkCreateShadowRenderPass(device, ctx.shadowformat, ctx.shadowrenderpass)) {
		gvkDestroyShadowResources(ctx);
		return false;
	}

	VkFramebufferCreateInfo fbinfo{};
	fbinfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbinfo.renderPass = ctx.shadowrenderpass;
	fbinfo.attachmentCount = 1;
	fbinfo.width = width;
	fbinfo.height = height;
	fbinfo.layers = 1;
	for(int frame = 0; frame < GVK_MAX_FRAMES_IN_FLIGHT; ++frame) {
		fbinfo.pAttachments = &ctx.shadowviews[frame];
		if(vkCreateFramebuffer(device, &fbinfo, nullptr, &ctx.shadowframebuffers[frame]) != VK_SUCCESS) {
			gLoge("gVKShadow") << "vkCreateFramebuffer failed for shadow frame " << frame << ".";
			gvkDestroyShadowResources(ctx);
			return false;
		}
	}

	// The set is shaped exactly like a texture's - one combined image sampler at
	// binding 0 - so the mesh shader can bind it in the same slot pattern it uses
	// for material maps, with no layout of its own.
	VkDescriptorSetLayout layout = ctx.getImageDescriptorSetLayout();
	if(layout != VK_NULL_HANDLE && ctx.descriptorpool != VK_NULL_HANDLE) {
		VkDescriptorSetAllocateInfo dsalloc{};
		dsalloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		dsalloc.descriptorPool = ctx.descriptorpool;
		std::vector<VkDescriptorSetLayout> layouts(GVK_MAX_FRAMES_IN_FLIGHT, layout);
		dsalloc.descriptorSetCount = GVK_MAX_FRAMES_IN_FLIGHT;
		dsalloc.pSetLayouts = layouts.data();
		if(vkAllocateDescriptorSets(device, &dsalloc, ctx.shadowdescriptorsets.data()) != VK_SUCCESS) {
			gLoge("gVKShadow") << "vkAllocateDescriptorSets failed for the shadow map.";
			gvkDestroyShadowResources(ctx);
			return false;
		}
		for(int frame = 0; frame < GVK_MAX_FRAMES_IN_FLIGHT; ++frame) {
			VkDescriptorImageInfo imginfo{};
			imginfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imginfo.imageView = ctx.shadowviews[frame];
			imginfo.sampler = ctx.shadowsampler;
			VkWriteDescriptorSet write{};
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.dstSet = ctx.shadowdescriptorsets[frame];
			write.dstBinding = 0;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.descriptorCount = 1;
			write.pImageInfo = &imginfo;
			vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
		}
	}

	// The pipeline is part of the same unit: it is built against the render pass
	// created above and is useless without it, so the two are made and freed
	// together rather than left to be sequenced by the caller.
	if(!gvkCreateShadowPipeline(ctx)) {
		gvkDestroyShadowResources(ctx);
		return false;
	}

	gLogi("gVKShadow") << "Shadow map created: " << width << "x" << height
			<< ", format " << ctx.shadowformat;
	return true;
}

void gvkDestroyShadowResources(gVKContext& ctx) {
	VkDevice device = *ctx.getDevice();
	if(device == VK_NULL_HANDLE) return;

	// The descriptor set is not freed on its own; it came from the pool that
	// gvkDestroyGraphicsPipelines destroys as a whole.
	ctx.shadowdescriptorsets.clear();

	// Before the render pass it was built against.
	gvkDestroyShadowPipeline(ctx);

	for(VkFramebuffer framebuffer : ctx.shadowframebuffers) {
		if(framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
	}
	ctx.shadowframebuffers.clear();
	if(ctx.shadowrenderpass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(device, ctx.shadowrenderpass, nullptr);
		ctx.shadowrenderpass = VK_NULL_HANDLE;
	}
	if(ctx.shadowsampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, ctx.shadowsampler, nullptr);
		ctx.shadowsampler = VK_NULL_HANDLE;
	}
	for(VkImageView view : ctx.shadowviews) if(view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
	for(VkImage image : ctx.shadowimages) if(image != VK_NULL_HANDLE) vkDestroyImage(device, image, nullptr);
	for(VkDeviceMemory memory : ctx.shadowmemories) if(memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
	ctx.shadowviews.clear();
	ctx.shadowimages.clear();
	ctx.shadowmemories.clear();
	ctx.shadowextent = {0, 0};
}

bool gvkBeginShadowPass(gVKContext& ctx) {
	if(!ctx.frameactive || ctx.renderpassactive) return false;
	if(ctx.currentframe >= ctx.shadowframebuffers.size()
			|| ctx.shadowframebuffers[ctx.currentframe] == VK_NULL_HANDLE) return false;

	VkCommandBuffer cmd = ctx.commandbuffers[ctx.currentframe];

	VkClearValue clear{};
	clear.depthStencil = {1.0f, 0};

	VkRenderPassBeginInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	info.renderPass = ctx.shadowrenderpass;
	info.framebuffer = ctx.shadowframebuffers[ctx.currentframe];
	info.renderArea.offset = {0, 0};
	info.renderArea.extent = ctx.shadowextent;
	info.clearValueCount = 1;
	info.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
	ctx.resetRecordedDrawState();

	ctx.renderpassactive = true;
	ctx.shadowpassactive = true;
	return true;
}

void gvkEndShadowPass(gVKContext& ctx) {
	if(!ctx.shadowpassactive) return;
	vkCmdEndRenderPass(ctx.commandbuffers[ctx.currentframe]);
	ctx.renderpassactive = false;
	ctx.shadowpassactive = false;
}

#endif /* GVK_DESKTOP_GLFW */
