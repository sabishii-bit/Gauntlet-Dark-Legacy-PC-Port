#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Paths.h"
#include "engine/platform/Window.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/vulkan/VulkanCommon.h"
#include "engine/render/vulkan/VulkanContext.h"
#include "engine/render/vulkan/VulkanPipeline.h"
#include "engine/render/vulkan/VulkanTexture.h"

namespace {

using namespace gdl;
constexpr u32 kSize = 8;
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

struct Target {
    VulkanContext& context;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    Target(VulkanContext& device, VkFormat format, VkSampleCountFlagBits samples, bool depth)
        : context(device) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {kSize, kSize, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                           : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        GDL_VK_CHECK(
            vmaCreateImage(context.allocator(), &info, &memory, &image, &allocation, nullptr));
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {
            static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                  : VK_IMAGE_ASPECT_COLOR_BIT),
            0, 1, 0, 1};
        GDL_VK_CHECK(vkCreateImageView(context.device(), &viewInfo, nullptr, &view));
    }
    ~Target() {
        vkDestroyImageView(context.device(), view, nullptr);
        vmaDestroyImage(context.allocator(), image, allocation);
    }
    GDL_NON_COPYABLE_NON_MOVABLE(Target);
};

struct Buffer {
    VulkanContext& context;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo mapped{};

    Buffer(VulkanContext& device, VkDeviceSize size, VkBufferUsageFlags usage) : context(device) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        memory.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        GDL_VK_CHECK(
            vmaCreateBuffer(context.allocator(), &info, &memory, &buffer, &allocation, &mapped));
    }
    ~Buffer() { vmaDestroyBuffer(context.allocator(), buffer, allocation); }
    GDL_NON_COPYABLE_NON_MOVABLE(Buffer);
};

struct CoverageDevice {
    std::unique_ptr<Window> window = createGlfwWindow({"gdl coverage readback test", 320, 240});
    VulkanContext context{*window, true};
    VkDescriptorSetLayout imageLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout samplerLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet samplerSet = VK_NULL_HANDLE;

    CoverageDevice() {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings = &binding;
        GDL_VK_CHECK(vkCreateDescriptorSetLayout(context.device(), &info, nullptr, &imageLayout));
        binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        GDL_VK_CHECK(vkCreateDescriptorSetLayout(context.device(), &info, nullptr, &samplerLayout));
        const std::array<VkDescriptorPoolSize, 2> sizes{
            {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 3;
        poolInfo.poolSizeCount = static_cast<u32>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        GDL_VK_CHECK(vkCreateDescriptorPool(context.device(), &poolInfo, nullptr, &pool));
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.maxLod = 0;
        GDL_VK_CHECK(vkCreateSampler(context.device(), &samplerInfo, nullptr, &sampler));
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &samplerLayout;
        GDL_VK_CHECK(vkAllocateDescriptorSets(context.device(), &allocate, &samplerSet));
        const VkDescriptorImageInfo sampled{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = samplerSet;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &sampled;
        vkUpdateDescriptorSets(context.device(), 1, &write, 0, nullptr);
    }
    ~CoverageDevice() {
        vkDestroyDescriptorPool(context.device(), pool, nullptr);
        vkDestroySampler(context.device(), sampler, nullptr);
        vkDestroyDescriptorSetLayout(context.device(), samplerLayout, nullptr);
        vkDestroyDescriptorSetLayout(context.device(), imageLayout, nullptr);
    }
    GDL_NON_COPYABLE_NON_MOVABLE(CoverageDevice);

    std::vector<u8> render(VkSampleCountFlagBits samples, u8 alpha, bool masked) {
        const Target color(context, kColorFormat, samples, false);
        const Target resolved(context, kColorFormat, VK_SAMPLE_COUNT_1_BIT, false);
        const Target depth(context, context.depthFormat(), samples, true);
        const std::array<u8, 4> pixel{255, 255, 255, alpha};
        const std::array<u8, 4> skinPixel{255, 255, 255, 64};
        const VulkanTexture texture(context, pool, imageLayout, TextureDesc{1, 1}, pixel);
        const VulkanTexture skin(context, pool, imageLayout, TextureDesc{1, 1}, skinPixel);
        const auto shaders = paths::executableDirectory() / "shaders";
        const VulkanPipeline cutout(context, shaders, kColorFormat, context.depthFormat(),
                                    imageLayout, BlendMode::Opaque, samples,
                                    VulkanPipeline::Effect::None, samplerLayout, true);
        const VulkanPipeline solid(context, shaders, kColorFormat, context.depthFormat(),
                                   imageLayout, BlendMode::Opaque, samples,
                                   VulkanPipeline::Effect::None, samplerLayout);
        ImmediateBatch geometry;
        geometry.rect({0, 0, kSize, kSize}, 0.75f, Color::rgba(255, 0, 0));
        geometry.rect({0, 0, kSize, kSize}, 0.25f, Color::rgba(0, 255, 0));
        const auto vertices = geometry.triangles();
        const Buffer vertexBuffer(context, vertices.size_bytes(),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        std::memcpy(vertexBuffer.mapped.pMappedData, vertices.data(), vertices.size_bytes());
        GDL_VK_CHECK(
            vmaFlushAllocation(context.allocator(), vertexBuffer.allocation, 0, VK_WHOLE_SIZE));
        constexpr usize kPixelBytes = usize{kSize} * kSize * 4;
        const Buffer readback(context, kPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto* const cmd = context.beginOneShotCommands();
        for (const auto* target : {&color, &resolved}) {
            vk::imageBarrier(cmd, target->image, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
        vk::imageBarrier(cmd, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo colorInfo{};
        colorInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorInfo.imageView = color.view;
        colorInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorInfo.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (samples != VK_SAMPLE_COUNT_1_BIT) {
            colorInfo.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            colorInfo.resolveImageView = resolved.view;
            colorInfo.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkRenderingAttachmentInfo depthInfo{};
        depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthInfo.imageView = depth.view;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthInfo.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = {kSize, kSize};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorInfo;
        rendering.pDepthAttachment = &depthInfo;
        vkCmdBeginRendering(cmd, &rendering);
        const VkViewport viewport{0, 0, kSize, kSize, 0, 1};
        const VkRect2D scissor{{0, 0}, {kSize, kSize}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
        vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_GREATER_OR_EQUAL);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.buffer, &offset);
        const std::array<VkDescriptorSet, 4> sets{texture.descriptorSet(), skin.descriptorSet(),
                                                  samplerSet, samplerSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cutout.layout(), 0,
                                static_cast<u32>(sets.size()), sets.data(), 0, nullptr);
        VulkanPipeline::PushConstants constants{makeScreenProjection(kSize, kSize),
                                                Vec4{0, 0, DrawState::kTranslucentAlphaTest, 0},
                                                Vec4{1, 1, masked ? 1.0f : 0.0f, 1}};
        // Lightmap alpha is irrelevant here: use the frame-blend path at 100% to select
        // the base image for the ordinary test without a second-stage intensity.
        if (!masked) {
            const std::array<VkDescriptorSet, 4> same{
                texture.descriptorSet(), texture.descriptorSet(), samplerSet, samplerSet};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cutout.layout(), 0,
                                    static_cast<u32>(same.size()), same.data(), 0, nullptr);
            constants.scale.z = -1;
        }
        const auto draw = [&](const VulkanPipeline& pipeline, u32 first) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
            vkCmdPushConstants(cmd, pipeline.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               VulkanPipeline::kPushConstantSize, &constants);
            vkCmdDraw(cmd, 6, 1, first, 0);
        };
        draw(cutout, 0);
        constants.params.z = 0;
        constants.scale.z = -1; // the behind surface is solid regardless of texture alpha
        draw(solid, 6);
        vkCmdEndRendering(cmd);
        auto* const output = samples == VK_SAMPLE_COUNT_1_BIT ? color.image : resolved.image;
        vk::imageBarrier(
            cmd, output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kSize, kSize, 1};
        vkCmdCopyImageToBuffer(cmd, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                               1, &copy);
        VkMemoryBarrier2 hostRead{};
        hostRead.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        hostRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        hostRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hostRead.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostRead.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo ready{};
        ready.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        ready.memoryBarrierCount = 1;
        ready.pMemoryBarriers = &hostRead;
        vkCmdPipelineBarrier2(cmd, &ready);
        context.endOneShotCommands(cmd);
        GDL_VK_CHECK(
            vmaInvalidateAllocation(context.allocator(), readback.allocation, 0, VK_WHOLE_SIZE));
        const std::span pixels{static_cast<const u8*>(readback.mapped.pMappedData), kPixelBytes};
        return {pixels.begin(), pixels.end()};
    }
};

TEST_CASE("GPU alpha coverage preserves cutout holes and does not multiply edge alpha twice",
          "[gpu][alpha-coverage]") {
    CoverageDevice gpu;
    const auto supported = gpu.context.properties().limits.framebufferColorSampleCounts &
                           gpu.context.properties().limits.framebufferDepthSampleCounts;
    for (const auto samples :
         {VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT}) {
        if ((supported & samples) == 0) {
            continue;
        }
        for (const bool masked : {false, true}) {
            for (const u8 alpha : {u8{0}, u8{1}, u8{128}, u8{255}}) {
                CAPTURE(samples, masked, alpha);
                const auto pixels = gpu.render(samples, alpha, masked);
                for (usize i = 0; i < pixels.size(); i += 4) {
                    const u8 red = pixels[i];
                    const u8 green = pixels[i + 1];
                    CHECK(pixels[i + 2] == 0);
                    CHECK(static_cast<u32>(red) + green >= 254);
                    if (alpha < 3) {
                        CHECK(red == 0);
                        CHECK(green == 255);
                    } else if (alpha == 255 || samples == VK_SAMPLE_COUNT_1_BIT) {
                        CHECK(red == 255);
                        CHECK(green == 0);
                    } else {
                        CHECK(red > 0);
                        CHECK(red < 255);
                        CHECK(green > 0);
                    }
                }
            }
        }
    }
}

} // namespace
