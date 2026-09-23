#include "engine/render/vulkan/VulkanRenderDevice.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/platform/Window.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/vulkan/VulkanContext.h"
#include "engine/render/vulkan/VulkanPipeline.h"
#include "engine/render/vulkan/VulkanSwapchain.h"
#include "engine/render/vulkan/VulkanTexture.h"

namespace gdl {

namespace {

constexpr VkDeviceSize kUploadAlignment = 16;

VkSampler createSampler(VkDevice device, VkFilter filter, VkSamplerAddressMode across,
                        VkSamplerAddressMode down) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = across;
    info.addressModeV = down;
    info.addressModeW = across;
    info.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    GDL_VK_CHECK(vkCreateSampler(device, &info, nullptr, &sampler));
    return sampler;
}

} // namespace

VulkanRenderDevice::VulkanRenderDevice(Window& window, const RenderDeviceDesc& desc)
    : m_window(window) {
    m_context = std::make_unique<VulkanContext>(window, desc.enableValidation);
    m_swapchain =
        std::make_unique<VulkanSwapchain>(*m_context, window.framebufferSize(), desc.vsync);

    createDescriptorResources();
    m_pipeline = std::make_unique<VulkanPipeline>(
        *m_context, desc.shaderDirectory, m_swapchain->colorFormat(), m_swapchain->depthFormat(),
        m_textureSetLayout, BlendMode::Alpha);
    m_additivePipeline = std::make_unique<VulkanPipeline>(
        *m_context, desc.shaderDirectory, m_swapchain->colorFormat(), m_swapchain->depthFormat(),
        m_textureSetLayout, BlendMode::Additive);
    m_opaquePipeline = std::make_unique<VulkanPipeline>(
        *m_context, desc.shaderDirectory, m_swapchain->colorFormat(), m_swapchain->depthFormat(),
        m_textureSetLayout, BlendMode::Opaque);
    createFrameResources();
    createPresentSemaphores();

    constexpr std::array<u8, 4> kWhitePixel{255, 255, 255, 255};
    m_whiteTexture = std::make_unique<VulkanTexture>(
        *m_context, descriptorPoolForTexture(), m_textureSetLayout,
        samplerFor(TextureDesc{1, 1, TextureFilter::Nearest}), TextureDesc{1, 1}, kWhitePixel);

    log::info("Vulkan render device ready ({} frames in flight)", kFramesInFlight);
}

VulkanRenderDevice::~VulkanRenderDevice() {
    m_context->waitIdle();
    const VkDevice device = m_context->device();

    m_whiteTexture.reset();
    destroyPresentSemaphores();
    for (FrameResources& frame : m_frames) {
        destroyUploadBuffer(frame);
        if (frame.vertexBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_context->allocator(), frame.vertexBuffer, frame.vertexAllocation);
        }
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(device, frame.inFlight, nullptr);
        }
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, frame.imageAvailable, nullptr);
        }
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, frame.commandPool, nullptr);
        }
    }
    m_additivePipeline.reset();
    m_opaquePipeline.reset();
    m_pipeline.reset();
    for (VkSampler& sampler : m_samplers) {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
            sampler = VK_NULL_HANDLE;
        }
    }
    for (const VkDescriptorPool pool : m_descriptorPools) {
        vkDestroyDescriptorPool(device, pool, nullptr);
    }
    m_descriptorPools.clear();
    if (m_textureSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_textureSetLayout, nullptr);
    }
    m_swapchain.reset();
    m_context.reset();
}

void VulkanRenderDevice::createDescriptorResources() {
    const VkDevice device = m_context->device();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    GDL_VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_textureSetLayout));

    m_poolTexturesLeft = 0;

    // One sampler for each filter and each way of wrapping across and down.
    const auto modeOf = [](TextureWrap wrap) {
        return wrap == TextureWrap::ClampToEdge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                                : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    for (const TextureFilter filter : {TextureFilter::Linear, TextureFilter::Nearest}) {
        for (const TextureWrap across : {TextureWrap::Repeat, TextureWrap::ClampToEdge}) {
            for (const TextureWrap down : {TextureWrap::Repeat, TextureWrap::ClampToEdge}) {
                m_samplers[samplerIndex(filter, across, down)] = createSampler(
                    device, filter == TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                    modeOf(across), modeOf(down));
            }
        }
    }
}

void VulkanRenderDevice::createFrameResources() {
    const VkDevice device = m_context->device();

    for (FrameResources& frame : m_frames) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = m_context->graphicsQueueFamily();
        GDL_VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &frame.commandPool));

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        GDL_VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &frame.commandBuffer));

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GDL_VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.imageAvailable));

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        GDL_VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlight));

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = VkDeviceSize{kMaxVerticesPerFrame} * sizeof(ImmediateVertex);
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocationInfo{};
        GDL_VK_CHECK(vmaCreateBuffer(m_context->allocator(), &bufferInfo, &allocCreate,
                                     &frame.vertexBuffer, &frame.vertexAllocation,
                                     &allocationInfo));
        frame.vertexMapped = allocationInfo.pMappedData;
        GDL_VERIFY(frame.vertexMapped != nullptr, "Per-frame vertex buffer is not host mapped");
    }
}

void VulkanRenderDevice::destroyUploadBuffer(FrameResources& frame) {
    if (frame.uploadBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_context->allocator(), frame.uploadBuffer, frame.uploadAllocation);
        frame.uploadBuffer = VK_NULL_HANDLE;
        frame.uploadAllocation = VK_NULL_HANDLE;
        frame.uploadMapped = nullptr;
        frame.uploadCapacity = 0;
    }
}

void VulkanRenderDevice::reserveUploadBuffer(FrameResources& frame, VkDeviceSize bytes) {
    if (frame.uploadCapacity >= bytes) {
        return;
    }
    destroyUploadBuffer(frame);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = std::max(bytes, frame.uploadCapacity * 2);
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreate.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo allocationInfo{};
    GDL_VK_CHECK(vmaCreateBuffer(m_context->allocator(), &bufferInfo, &allocCreate,
                                 &frame.uploadBuffer, &frame.uploadAllocation, &allocationInfo));
    frame.uploadMapped = allocationInfo.pMappedData;
    frame.uploadCapacity = bufferInfo.size;
    GDL_VERIFY(frame.uploadMapped != nullptr, "Texture upload buffer is not host mapped");
}

void VulkanRenderDevice::createPresentSemaphores() {
    const VkDevice device = m_context->device();
    m_renderFinished.resize(m_swapchain->imageCount(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : m_renderFinished) {
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GDL_VK_CHECK(vkCreateSemaphore(device, &info, nullptr, &semaphore));
    }
}

void VulkanRenderDevice::destroyPresentSemaphores() {
    for (const VkSemaphore semaphore : m_renderFinished) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_context->device(), semaphore, nullptr);
        }
    }
    m_renderFinished.clear();
}

void VulkanRenderDevice::recreateSwapchain() {
    const Extent2D size = m_window.framebufferSize();
    if (size.isZero()) {
        return;
    }
    m_swapchain->recreate(size);
    destroyPresentSemaphores();
    createPresentSemaphores();
}

Extent2D VulkanRenderDevice::framebufferExtent() const {
    const VkExtent2D extent = m_swapchain->extent();
    return Extent2D{extent.width, extent.height};
}

bool VulkanRenderDevice::beginFrame() {
    GDL_ASSERT(!m_frameOpen, "beginFrame called twice without endFrame");

    const Extent2D windowSize = m_window.framebufferSize();
    if (windowSize.isZero()) {
        return false;
    }
    if (windowSize != framebufferExtent()) {
        recreateSwapchain();
    }

    const VkDevice device = m_context->device();
    FrameResources& frame = m_frames[m_frameIndex];

    GDL_VK_CHECK(
        vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, std::numeric_limits<u64>::max()));

    const VkResult acquire = m_swapchain->acquireNextImage(frame.imageAvailable, &m_imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return false;
    }
    GDL_VERIFY(acquire == VK_SUCCESS || acquire == VK_SUBOPTIMAL_KHR,
               "vkAcquireNextImageKHR failed");

    GDL_VK_CHECK(vkResetFences(device, 1, &frame.inFlight));
    GDL_VK_CHECK(vkResetCommandPool(device, frame.commandPool, 0));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    GDL_VK_CHECK(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo));

    const VkCommandBuffer cmd = frame.commandBuffer;
    vk::imageBarrier(
        cmd, m_swapchain->image(m_imageIndex), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    vk::imageBarrier(
        cmd, m_swapchain->depthImage(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        0,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    frame.vertexCursor = 0;
    frame.uploadCursor = 0;
    m_frameOpen = true;
    m_renderingStarted = false;
    return true;
}

void VulkanRenderDevice::beginRendering() {
    const FrameResources& frame = m_frames[m_frameIndex];
    const VkCommandBuffer cmd = frame.commandBuffer;
    const VkExtent2D extent = m_swapchain->extent();

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = m_swapchain->imageView(m_imageIndex);
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {
        {m_clearColor.r, m_clearColor.g, m_clearColor.b, m_clearColor.a}};

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = m_swapchain->depthImageView();
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {0.0f, 0};

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = extent;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<f32>(extent.width);
    viewport.height = static_cast<f32>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
    m_boundBlend = BlendMode::Alpha;
    const VkDeviceSize zeroOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &frame.vertexBuffer, &zeroOffset);
    m_renderingStarted = true;
}

void VulkanRenderDevice::updateTexture(Texture& texture, std::span<const u8> rgba8Pixels) {
    GDL_VERIFY(m_frameOpen && !m_renderingStarted,
               "updateTexture must be called after beginFrame and before the frame's first draw");
    auto& vulkanTexture = dynamic_cast<VulkanTexture&>(texture);
    const VkDeviceSize bytes = VkDeviceSize{vulkanTexture.width()} * vulkanTexture.height() * 4;
    GDL_VERIFY(rgba8Pixels.size() == bytes,
               "updateTexture pixel data size does not match the texture");

    FrameResources& frame = m_frames[m_frameIndex];
    const VkDeviceSize offset =
        (frame.uploadCursor + kUploadAlignment - 1) & ~(kUploadAlignment - 1);
    reserveUploadBuffer(frame, offset + bytes);
    const std::span<u8> mapped(static_cast<u8*>(frame.uploadMapped),
                               static_cast<usize>(frame.uploadCapacity));
    std::memcpy(mapped.subspan(static_cast<usize>(offset), rgba8Pixels.size()).data(),
                rgba8Pixels.data(), rgba8Pixels.size());
    GDL_VK_CHECK(vmaFlushAllocation(m_context->allocator(), frame.uploadAllocation, offset, bytes));
    frame.uploadCursor = offset + bytes;

    const VkCommandBuffer cmd = frame.commandBuffer;
    const VkImage image = vulkanTexture.image();
    vk::imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {vulkanTexture.width(), vulkanTexture.height(), 1};
    vkCmdCopyBufferToImage(cmd, frame.uploadBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);

    vk::imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VulkanRenderDevice::draw(const ImmediateBatch& batch, const Texture& texture,
                              const Mat4& transform, const DrawState& state) {
    GDL_ASSERT(m_frameOpen, "draw called outside beginFrame/endFrame");
    if (!m_renderingStarted) {
        beginRendering();
    }
    if (state.blend != m_boundBlend) {
        const VulkanPipeline* pipeline = m_pipeline.get();
        if (state.blend == BlendMode::Opaque) {
            pipeline = m_opaquePipeline.get();
        } else if (state.blend == BlendMode::Additive) {
            pipeline = m_additivePipeline.get();
        }
        vkCmdBindPipeline(m_frames[m_frameIndex].commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline->handle());
        m_boundBlend = state.blend;
    }
    const auto triangles = batch.triangles();
    if (triangles.empty()) {
        return;
    }

    FrameResources& frame = m_frames[m_frameIndex];
    const auto count = static_cast<u32>(triangles.size());
    if (frame.vertexCursor + count > kMaxVerticesPerFrame) {
        if (!m_vertexOverflowReported) {
            log::warn("Immediate vertex buffer overflow ({} vertices per frame); draw skipped",
                      kMaxVerticesPerFrame);
            m_vertexOverflowReported = true;
        }
        return;
    }

    const std::span<ImmediateVertex> vertexBuffer(static_cast<ImmediateVertex*>(frame.vertexMapped),
                                                  kMaxVerticesPerFrame);
    std::memcpy(vertexBuffer.subspan(frame.vertexCursor, count).data(), triangles.data(),
                triangles.size_bytes());

    const VkCommandBuffer cmd = frame.commandBuffer;
    GDL_ASSERT(state.maskedTexture == nullptr || state.lightmap == nullptr,
               "masked colour and lightmap share the second texture stage");
    const Texture* second = m_whiteTexture.get();
    if (state.maskedTexture != nullptr) {
        second = state.maskedTexture;
    } else if (state.lightmap != nullptr) {
        second = state.lightmap;
    }
    const std::array<VkDescriptorSet, 2> sets{
        dynamic_cast<const VulkanTexture&>(texture).descriptorSet(),
        dynamic_cast<const VulkanTexture&>(*second).descriptorSet()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->layout(), 0,
                            static_cast<u32>(sets.size()), sets.data(), 0, nullptr);
    const VulkanPipeline::PushConstants constants{
        transform, Vec4{state.uvOffset.x, state.uvOffset.y, state.alphaTest, state.darken},
        Vec4{state.uvScale.x, state.uvScale.y, state.maskedTexture != nullptr ? 1.0f : 0.0f, 0.0f}};
    vkCmdPushConstants(cmd, m_pipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       VulkanPipeline::kPushConstantSize, &constants);
    // The additive pipeline never writes depth; the others do unless the draw says not to.
    vkCmdSetCullMode(cmd, state.cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    vkCmdSetDepthWriteEnable(
        cmd, (state.depthWrite && state.blend != BlendMode::Additive) ? VK_TRUE : VK_FALSE);
    vkCmdDraw(cmd, count, 1, frame.vertexCursor, 0);

    frame.vertexCursor += count;
}

void VulkanRenderDevice::endFrame() {
    GDL_ASSERT(m_frameOpen, "endFrame called without beginFrame");
    if (!m_renderingStarted) {
        beginRendering();
    }
    m_frameOpen = false;
    m_renderingStarted = false;

    const FrameResources& frame = m_frames[m_frameIndex];
    const VkCommandBuffer cmd = frame.commandBuffer;

    if (frame.vertexCursor > 0) {
        GDL_VK_CHECK(
            vmaFlushAllocation(m_context->allocator(), frame.vertexAllocation, 0,
                               VkDeviceSize{frame.vertexCursor} * sizeof(ImmediateVertex)));
    }

    vkCmdEndRendering(cmd);
    vk::imageBarrier(cmd, m_swapchain->image(m_imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    GDL_VK_CHECK(vkEndCommandBuffer(cmd));

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = m_renderFinished[m_imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandInfo.commandBuffer = cmd;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitInfo;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;
    GDL_VK_CHECK(vkQueueSubmit2(m_context->graphicsQueue(), 1, &submitInfo, frame.inFlight));

    const VkResult present = m_swapchain->present(m_renderFinished[m_imageIndex], m_imageIndex);
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    } else {
        GDL_VERIFY(present == VK_SUCCESS, "vkQueuePresentKHR failed");
    }

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

/** The pool the next texture's descriptor set comes from: a fresh one whenever the last is
 * full, since a level and the archives it borrows from can hold thousands of textures. */
VkDescriptorPool VulkanRenderDevice::descriptorPoolForTexture() {
    if (m_poolTexturesLeft == 0) {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = kTexturesPerPool;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = kTexturesPerPool;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        GDL_VK_CHECK(vkCreateDescriptorPool(m_context->device(), &poolInfo, nullptr, &pool));
        m_descriptorPools.push_back(pool);
        m_poolTexturesLeft = kTexturesPerPool;
    }
    --m_poolTexturesLeft;
    return m_descriptorPools.back();
}

std::unique_ptr<Texture> VulkanRenderDevice::createTexture(const TextureDesc& desc,
                                                           std::span<const u8> rgba8Pixels) {
    return std::make_unique<VulkanTexture>(*m_context, descriptorPoolForTexture(),
                                           m_textureSetLayout, samplerFor(desc), desc, rgba8Pixels);
}

const Texture& VulkanRenderDevice::whiteTexture() const {
    return *m_whiteTexture;
}

void VulkanRenderDevice::waitIdle() {
    m_context->waitIdle();
}

std::unique_ptr<RenderDevice> createVulkanRenderDevice(Window& window,
                                                       const RenderDeviceDesc& desc) {
    return std::make_unique<VulkanRenderDevice>(window, desc);
}

} // namespace gdl
