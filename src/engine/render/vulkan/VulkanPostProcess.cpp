#include "engine/render/vulkan/VulkanPostProcess.h"

#include <algorithm>

#include "engine/render/vulkan/VulkanContext.h"
#include "engine/render/vulkan/VulkanPipeline.h"
#include "engine/render/vulkan/VulkanSwapchain.h"

namespace gdl {
VulkanPostProcess::VulkanPostProcess(VulkanContext& context, const VulkanSwapchain& swapchain,
                                     const std::filesystem::path& shaders,
                                     VkDescriptorSetLayout textureLayout, VkSampler colorSampler,
                                     VkSampler depthSampler)
    : m_context(context) {
    const auto extent = swapchain.extent();
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = swapchain.colorFormat();
    image.extent = {extent.width, extent.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    GDL_VK_CHECK(
        vmaCreateImage(context.allocator(), &image, &allocation, &m_color, &m_allocation, nullptr));
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = m_color;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    GDL_VK_CHECK(vkCreateImageView(context.device(), &view, nullptr, &m_view));
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 2;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    GDL_VK_CHECK(vkCreateDescriptorPool(context.device(), &pool, nullptr, &m_pool));
    const std::array layouts{textureLayout, textureLayout};
    VkDescriptorSetAllocateInfo sets{};
    sets.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    sets.descriptorPool = m_pool;
    sets.descriptorSetCount = 2;
    sets.pSetLayouts = layouts.data();
    GDL_VK_CHECK(vkAllocateDescriptorSets(context.device(), &sets, m_sets.data()));
    const std::array<VkDescriptorImageInfo, 2> images{
        VkDescriptorImageInfo{colorSampler, m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{depthSampler, swapchain.depthImageView(),
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_sets[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(context.device(), static_cast<u32>(writes.size()), writes.data(), 0,
                           nullptr);
    m_pipeline = std::make_unique<VulkanPipeline>(
        context, shaders, swapchain.colorFormat(), VK_FORMAT_UNDEFINED, textureLayout,
        BlendMode::Opaque, swapchain.samples(), VulkanPipeline::Effect::DepthOfField);
    m_bloomPipeline = std::make_unique<VulkanPipeline>(
        context, shaders, swapchain.colorFormat(), VK_FORMAT_UNDEFINED, textureLayout,
        BlendMode::Opaque, swapchain.samples(), VulkanPipeline::Effect::Bloom);
    m_aoPipeline = std::make_unique<VulkanPipeline>(
        context, shaders, swapchain.colorFormat(), VK_FORMAT_UNDEFINED, textureLayout,
        BlendMode::Opaque, swapchain.samples(), VulkanPipeline::Effect::AmbientOcclusion);
}

VulkanPostProcess::~VulkanPostProcess() {
    m_pipeline.reset();
    m_bloomPipeline.reset();
    m_aoPipeline.reset();
    vkDestroyDescriptorPool(m_context.device(), m_pool, nullptr);
    vkDestroyImageView(m_context.device(), m_view, nullptr);
    vmaDestroyImage(m_context.allocator(), m_color, m_allocation);
}

void VulkanPostProcess::record(VkCommandBuffer cmd, const VulkanSwapchain& swapchain,
                               u32 imageIndex, const DepthOfField& settings) {
    const f32 width = static_cast<f32>(swapchain.extent().width);
    const f32 height = static_cast<f32>(swapchain.extent().height);
    const VulkanPipeline::PushConstants constants{
        settings.clipToView,
        Vec4{settings.focusEnd, std::max(settings.transition, 0.001f),
             std::clamp(settings.radiusAt1080, 0.0f, 8.0f) * height / 1080.0f,
             static_cast<f32>(swapchain.samples())},
        Vec4{1.0f / width, 1.0f / height, 0.0f, 0.0f}};
    recordPass(cmd, swapchain, imageIndex, *m_pipeline, constants);
}

void VulkanPostProcess::recordBloom(VkCommandBuffer cmd, const VulkanSwapchain& swapchain,
                                    u32 imageIndex, const HeatDistortion& heat) {
    const f32 width = static_cast<f32>(swapchain.extent().width);
    const f32 height = static_cast<f32>(swapchain.extent().height);
    // Deliberately restrained LDR bloom: bright scene pixels, not an emissive material mask.
    // The five-by-five kernel has a six-pixel radius at 1080p, independent of frame rate.
    VulkanPipeline::PushConstants constants{
        Mat4{0.0f}, Vec4{0.6f, 0.9f, 0.4f, 3.0f * height / 1080.0f},
        Vec4{1.0f / width, 1.0f / height, static_cast<f32>(swapchain.samples()), 0.0f}};
    for (usize i = 0; i < HeatDistortion::kCapacity; ++i) {
        const auto lane = static_cast<s32>(i);
        const auto& source = heat.sources()[i];
        constants.transform[lane] = Vec4{source.center, source.radius};
        constants.heatDepths[lane] = source.depth;
        constants.heatTimes[lane] = source.seconds;
    }
    recordPass(cmd, swapchain, imageIndex, *m_bloomPipeline, constants);
}

void VulkanPostProcess::recordAmbientOcclusion(VkCommandBuffer cmd,
                                               const VulkanSwapchain& swapchain, u32 imageIndex,
                                               const AmbientOcclusion& settings) {
    const VulkanPipeline::PushConstants constants{
        settings.clipToView,
        Vec4{std::clamp(settings.radius, 0.01f, 5.0f), std::max(settings.bias, 0.001f),
             std::clamp(settings.strength, 0.0f, 0.35f), static_cast<f32>(swapchain.samples())},
        Vec4{1.0f / static_cast<f32>(swapchain.extent().width),
             1.0f / static_cast<f32>(swapchain.extent().height), 0.0f, 0.0f}};
    recordPass(cmd, swapchain, imageIndex, *m_aoPipeline, constants);
}

void VulkanPostProcess::recordPass(VkCommandBuffer cmd, const VulkanSwapchain& swapchain,
                                   u32 imageIndex, const VulkanPipeline& pipeline,
                                   const VulkanPipeline::PushConstants& constants) {
    const auto extent = swapchain.extent();
    const VkImage scene = swapchain.image(imageIndex);
    vk::imageBarrier(cmd, scene, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
    vk::imageBarrier(cmd, m_color, VK_IMAGE_ASPECT_COLOR_BIT,
                     m_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {extent.width, extent.height, 1};
    vkCmdCopyImage(cmd, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_color,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    m_initialized = true;
    vk::imageBarrier(cmd, m_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vk::imageBarrier(cmd, scene, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    constexpr auto kDepthStages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    vk::imageBarrier(cmd, swapchain.depthImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kDepthStages,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (swapchain.samples() != VK_SAMPLE_COUNT_1_BIT) {
        vk::imageBarrier(
            cmd, swapchain.multisampleImage(), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = swapchain.imageView(imageIndex);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (swapchain.samples() != VK_SAMPLE_COUNT_1_BIT) {
        color.imageView = swapchain.multisampleImageView();
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = swapchain.imageView(imageIndex);
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0,
                            static_cast<u32>(m_sets.size()), m_sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, pipeline.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       VulkanPipeline::kPushConstantSize, &constants);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    vk::imageBarrier(
        cmd, swapchain.depthImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kDepthStages,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    for (const VkImage image : {scene, swapchain.multisampleImage()}) {
        if (image != VK_NULL_HANDLE) {
            vk::imageBarrier(
                cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
    }
}
} // namespace gdl
