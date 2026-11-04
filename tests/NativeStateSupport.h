#pragma once

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"

#include "game/level/NativeStateResources.h"

namespace gdl::test {

/** The native digest an owner gives now. */
template <class Owner>
NativeStateDigest::Digest liveDigest(const Owner& owner,
                                     NativeStateDigest::Resources resources = {}) {
    NativeStateDigest digest{resources};
    REQUIRE(owner.appendNativeDigest(digest));
    const auto result = digest.checkedFinish();
    REQUIRE(result);
    return *result;
}

/** The same, over resources the owner registers first, for two loads that must agree. */
template <class Owner>
NativeStateDigest::Digest liveDigest(const Owner& owner, game::NativeStateResources& resources) {
    REQUIRE(owner.registerNativeResources(resources));
    return liveDigest(owner, resources.resources());
}

/** The digest of `saved` as `owner` would restore it. */
template <class Owner>
NativeStateDigest::Digest savedDigest(const Owner& owner,
                                      const typename Owner::LocalSnapshot& saved,
                                      NativeStateDigest::Resources resources = {}) {
    NativeStateDigest digest{resources};
    REQUIRE(owner.appendLocalDigest(digest, saved));
    const auto result = digest.checkedFinish();
    REQUIRE(result);
    return *result;
}

/** The same, over resources the owner registers first. */
template <class Owner>
NativeStateDigest::Digest savedDigest(const Owner& owner,
                                      const typename Owner::LocalSnapshot& saved,
                                      game::NativeStateResources& resources) {
    REQUIRE(owner.registerNativeResources(resources));
    return savedDigest(owner, saved, resources.resources());
}

} // namespace gdl::test
