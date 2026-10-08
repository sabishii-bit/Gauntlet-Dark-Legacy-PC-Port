#include "engine/render/vulkan/VulkanTexture.h"

#include <cstring>
#include <vector>

#include "engine/core/Assert.h"
#include "engine/core/Types.h"
#include "engine/render/vulkan/VulkanContext.h"

namespace gdl {

VulkanTexture::VulkanTexture(VulkanContext& context, VkDescriptorPool descriptorPool,
                             VkDescriptorSetLayout setLayout, const TextureDesc& desc,
                             std::span<const u8> rgba8Pixels)
    : m_context(context), m_descriptorPool(descriptorPool), m_width(desc.width),
      m_height(desc.height), m_desc(desc),
      m_mipLevels(desc.generateMipmaps && context.canBlitTextureMips()
                      ? textureMipCount(desc.width, desc.height)
                      : desc.mipLevels) {
    GDL_VERIFY(desc.width > 0 && desc.height > 0, "Texture dimensions must be non-zero");
    GDL_VERIFY(desc.mipLevels > 0 && desc.mipLevels <= textureMipCount(desc.width, desc.height),
               "Texture mip count exceeds its dimensions");
    GDL_VERIFY(desc.width <= context.properties().limits.maxImageDimension2D &&
                   desc.height <= context.properties().limits.maxImageDimension2D,
               "Texture dimensions exceed device limits");
    const VkDeviceSize byteSize = textureMipBytes({desc.width, desc.height}, desc.mipLevels);
    GDL_VERIFY(rgba8Pixels.size() == byteSize,
               "Texture pixel data size does not match its mip chain");

    const VkDevice device = m_context.device();
    const VmaAllocator allocator = m_context.allocator();

    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = byteSize;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAlloc{};
    stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAlloc.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    VmaAllocationInfo stagingAllocationInfo{};
    GDL_VK_CHECK(vmaCreateBuffer(allocator, &stagingInfo, &stagingAlloc, &stagingBuffer,
                                 &stagingAllocation, &stagingAllocationInfo));
    std::memcpy(stagingAllocationInfo.pMappedData, rgba8Pixels.data(), rgba8Pixels.size_bytes());
    GDL_VK_CHECK(vmaFlushAllocation(allocator, stagingAllocation, 0, byteSize));

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {desc.width, desc.height, 1};
    imageInfo.mipLevels = m_mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (m_mipLevels > 1 && context.canBlitTextureMips()) {
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imageAlloc{};
    imageAlloc.usage = VMA_MEMORY_USAGE_AUTO;

    GDL_VK_CHECK(
        vmaCreateImage(allocator, &imageInfo, &imageAlloc, &m_image, &m_allocation, nullptr));

    const VkCommandBuffer cmd = m_context.beginOneShotCommands();

    vk::imageBarrier(cmd, m_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0,
                     m_mipLevels);

    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    for (u32 level = 0; level < desc.mipLevels; ++level) {
        const auto size = textureMipExtent({desc.width, desc.height}, level);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.imageExtent = {size.width, size.height, 1};
        regions.push_back(region);
        offset += VkDeviceSize{size.width} * size.height * 4;
    }
    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<u32>(regions.size()), regions.data());

    prepareForSampling(cmd, desc.mipLevels);

    m_context.endOneShotCommands(cmd);
    vmaDestroyBuffer(allocator, stagingBuffer, stagingAllocation);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = m_mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    GDL_VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &m_view));

    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = m_descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout;
    GDL_VK_CHECK(vkAllocateDescriptorSets(device, &setInfo, &m_descriptorSet));

    VkDescriptorImageInfo imageDescriptor{};
    imageDescriptor.imageView = m_view;
    imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageDescriptor;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void VulkanTexture::prepareForSampling(VkCommandBuffer cmd, u32 suppliedLevels) const {
    for (u32 level = 0; level < m_mipLevels; ++level) {
        const bool blit = level + 1 >= suppliedLevels && level + 1 < m_mipLevels;
        if (blit) {
            const auto source = textureMipExtent({m_width, m_height}, level);
            const auto target = textureMipExtent(source, 1);
            vk::imageBarrier(cmd, m_image, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_READ_BIT, level);
            VkImageBlit region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            region.srcOffsets[1] = {static_cast<s32>(source.width), static_cast<s32>(source.height),
                                    1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level + 1, 0, 1};
            region.dstOffsets[1] = {static_cast<s32>(target.width), static_cast<s32>(target.height),
                                    1};
            vkCmdBlitImage(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        }
        vk::imageBarrier(
            cmd, m_image, VK_IMAGE_ASPECT_COLOR_BIT,
            blit ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            blit ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, level);
    }
}

VulkanTexture::~VulkanTexture() {
    const VkDevice device = m_context.device();
    if (m_descriptorSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device, m_descriptorPool, 1, &m_descriptorSet);
    }
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_view, nullptr);
    }
    if (m_image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_context.allocator(), m_image, m_allocation);
    }
}

} // namespace gdl
