#pragma once

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"

namespace gdl::game {
/** A stage attachment, identified by its encounter-owned obstacle index. */
struct CombatArenaTarget {
    usize index = 0;
    Mat4 placement{1};
    bool active = false;
    bool operator==(const CombatArenaTarget&) const = default;
};

inline void visitNativeState(NativeStateDigest& digest, const CombatArenaTarget& state) {
    digest.fields<3>(state);
}
} // namespace gdl::game
