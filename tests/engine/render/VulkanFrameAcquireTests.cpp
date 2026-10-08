#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/render/vulkan/VulkanFrameAcquire.h"

namespace {
using namespace gdl;

TEST_CASE("frame acquisition bounds both waits and resumes after an unavailable image",
          "[render][frame-acquire]") {
    const auto unavailable = GENERATE(VK_TIMEOUT, VK_NOT_READY);
    s32 waits = 0;
    s32 acquisitions = 0;
    const auto wait = [&](u64 timeout) {
        CHECK(timeout == 100'000'000);
        ++waits;
        return waits == 1 ? VK_TIMEOUT : VK_SUCCESS;
    };
    const auto acquire = [&](u64 timeout) {
        CHECK(timeout == 100'000'000);
        ++acquisitions;
        return acquisitions == 1 ? unavailable : VK_SUCCESS;
    };
    CHECK(vk::acquireFrame(wait, acquire) == VK_TIMEOUT);
    CHECK(acquisitions == 0);
    CHECK(vk::acquireFrame(wait, acquire) == unavailable);
    CHECK(vk::acquireFrame(wait, acquire) == VK_SUCCESS);
    CHECK(waits == 3);
    CHECK(acquisitions == 2);
}

TEST_CASE("frame acquisition preserves device loss and swapchain results",
          "[render][frame-acquire]") {
    const auto result = GENERATE(VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_DATE_KHR, VK_SUBOPTIMAL_KHR);
    bool acquired = false;
    const auto acquire = [&](u64) {
        acquired = true;
        return result;
    };
    CHECK(vk::acquireFrame([](u64) { return VK_ERROR_DEVICE_LOST; }, acquire) ==
          VK_ERROR_DEVICE_LOST);
    CHECK_FALSE(acquired);
    CHECK(vk::acquireFrame([](u64) { return VK_SUCCESS; }, acquire) == result);
    CHECK(acquired);
}
} // namespace
