#pragma once

#include <vulkan/vulkan_core.h>

#include "engine/core/Types.h"

namespace gdl::vk {
/** Bounded waits keep the window message pump running when presentation stalls. */
inline constexpr u64 kFrameWaitNanoseconds = 100'000'000;

/** Acquire only after the previous submission releases its semaphore. A pending
 * result leaves the frame's fence and command pool untouched for the next attempt. */
template <typename Wait, typename Acquire> VkResult acquireFrame(Wait wait, Acquire acquire) {
    const VkResult fence = wait(kFrameWaitNanoseconds);
    return fence == VK_SUCCESS ? acquire(kFrameWaitNanoseconds) : fence;
}
} // namespace gdl::vk
