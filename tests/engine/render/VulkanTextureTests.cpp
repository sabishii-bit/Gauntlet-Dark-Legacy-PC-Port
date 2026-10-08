#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/platform/Window.h"
#include "engine/render/Image.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/vulkan/VulkanCommon.h"
#include "engine/render/vulkan/VulkanContext.h"
#include "engine/render/vulkan/VulkanTexture.h"

namespace {

using namespace gdl;

struct TextureDevice {
    GDL_NON_COPYABLE_NON_MOVABLE(TextureDevice);
    std::unique_ptr<Window> window = createGlfwWindow({"gdl mip readback test", 320, 240});
    VulkanContext context{*window, true};
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    TextureDevice() {
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        GDL_VK_CHECK(vkCreateDescriptorSetLayout(context.device(), &layoutInfo, nullptr, &layout));
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        GDL_VK_CHECK(vkCreateDescriptorPool(context.device(), &poolInfo, nullptr, &pool));
    }

    ~TextureDevice() {
        vkDestroyDescriptorPool(context.device(), pool, nullptr);
        vkDestroyDescriptorSetLayout(context.device(), layout, nullptr);
    }

    std::vector<u8> read(const VulkanTexture& texture) {
        const auto bytes =
            textureMipBytes({texture.width(), texture.height()}, texture.mipLevels());
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo mapped{};
        GDL_VK_CHECK(
            vmaCreateBuffer(context.allocator(), &info, &allocInfo, &buffer, &allocation, &mapped));
        const VkCommandBuffer cmd = context.beginOneShotCommands();
        vk::imageBarrier(cmd, texture.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, 0, texture.mipLevels());
        std::vector<VkBufferImageCopy> regions;
        VkDeviceSize offset = 0;
        for (u32 level = 0; level < texture.mipLevels(); ++level) {
            const auto size = textureMipExtent({texture.width(), texture.height()}, level);
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            region.imageExtent = {size.width, size.height, 1};
            regions.push_back(region);
            offset += VkDeviceSize{size.width} * size.height * 4;
        }
        vkCmdCopyImageToBuffer(cmd, texture.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                               static_cast<u32>(regions.size()), regions.data());
        vk::imageBarrier(cmd, texture.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, texture.mipLevels());
        context.endOneShotCommands(cmd); // fence completion makes the copy visible to the host
        GDL_VK_CHECK(vmaInvalidateAllocation(context.allocator(), allocation, 0, bytes));
        const std::span pixels{static_cast<const u8*>(mapped.pMappedData),
                               static_cast<usize>(bytes)};
        std::vector<u8> result(pixels.begin(), pixels.end());
        vmaDestroyBuffer(context.allocator(), buffer, allocation);
        return result;
    }
};

TEST_CASE("GPU mip generation preserves authored levels and completes only the missing tail",
          "[gpu][mipmaps]") {
    TextureDevice gpu;
    if (!gpu.context.canBlitTextureMips()) {
        SKIP("Device does not support linear RGBA8 blits");
    }
    TextureDesc desc{8, 4};
    std::vector<u8> supplied;
    std::vector<u8> expected;
    SECTION("authored second level is not regenerated from the red base") {
        desc.mipLevels = 2;
    }
    SECTION("thin non-power-of-two texture reaches one by one") {
        desc.width = 1;
        desc.height = 13;
    }
    desc.generateMipmaps = true;
    const auto count = textureMipCount(desc.width, desc.height);
    for (u32 level = 0; level < count; ++level) {
        const auto size = textureMipExtent({desc.width, desc.height}, level);
        const auto colour =
            desc.mipLevels == 2 && level > 0 ? Color::rgba(0, 255, 0) : Color::rgba(255, 0, 0);
        const auto image = Image::filled(size.width, size.height, colour);
        expected.insert(expected.end(), image.pixels.begin(), image.pixels.end());
        if (level < desc.mipLevels) {
            supplied.insert(supplied.end(), image.pixels.begin(), image.pixels.end());
        }
    }
    const VulkanTexture texture(gpu.context, gpu.pool, gpu.layout, desc, supplied);
    CHECK(texture.mipLevels() == count);
    CHECK(gpu.read(texture) == expected);
}

} // namespace
