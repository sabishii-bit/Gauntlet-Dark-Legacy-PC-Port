#pragma once

#include <algorithm>
#include <string_view>

#include "engine/core/Types.h"

#include "game/scenes/PlayScene.h"

namespace gdl::test {

/** The first sample whose running digest differs between two profiles; empty when none does.
 * A sample one profile has and the other lacks counts as differing. */
inline std::string_view firstDivergence(const game::PlayScene::NativeDigestProfile& a,
                                        const game::PlayScene::NativeDigestProfile& b) {
    const usize shared = std::min(a.samples.size(), b.samples.size());
    for (usize i = 0; i < shared; ++i) {
        const auto& left = a.samples[i];
        const auto& right = b.samples[i];
        if (left.owner != right.owner || left.runningDigest != right.runningDigest) {
            return left.owner;
        }
    }
    if (a.samples.size() != b.samples.size()) {
        return (a.samples.size() > shared ? a.samples : b.samples)[shared].owner;
    }
    return {};
}

} // namespace gdl::test
