#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"

#include "NativeStateSupport.h"
#include "game/level/NativeStateResources.h"

namespace {

using namespace gdl;
using game::NativeStateResources;

/** An owner whose native state is one word. */
struct Counter {
    struct LocalSnapshot {
        u32 value = 0;
    };
    u32 value = 0;

    bool appendNativeDigest(NativeStateDigest& digest) const {
        digest.word(value);
        return digest.good();
    }
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
        digest.word(saved.value);
        return digest.good();
    }
};

/** An owner that also names an image it borrows, which only a registered registry knows. */
struct Painter {
    struct LocalSnapshot {
        u32 value = 0;
    };
    Image image = Image::filled(1, 1, Color::rgba(1, 2, 3, 4));
    u32 value = 0;

    bool registerNativeResources(NativeStateResources& resources) const {
        return resources.registerImage(image, "test/native-state/image");
    }
    bool appendNativeDigest(NativeStateDigest& digest) const {
        digest.resource(&image);
        digest.word(value);
        return digest.good();
    }
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
        digest.resource(&image);
        digest.word(saved.value);
        return digest.good();
    }
};

/** The digest of a stream holding one word. */
NativeStateDigest::Digest wordDigest(u32 value) {
    NativeStateDigest digest;
    digest.word(value);
    return digest.finish();
}

TEST_CASE("native state helpers hash what an owner and a saved value append", "[test-support]") {
    const Counter counter{.value = 7};
    CHECK(test::liveDigest(counter) == wordDigest(7));
    CHECK(test::savedDigest(counter, {.value = 7}) == test::liveDigest(counter));
    CHECK(test::savedDigest(counter, {.value = 8}) == wordDigest(8));
}

TEST_CASE("native state helpers register an owner's resources before hashing it",
          "[test-support]") {
    const Painter painter{.value = 7};
    const NativeStateResources empty;
    NativeStateDigest unregistered{empty.resources()};
    CHECK_FALSE(painter.appendNativeDigest(unregistered));

    NativeStateResources resources;
    const auto live = test::liveDigest(painter, resources);
    CHECK(test::liveDigest(painter, resources.resources()) == live);
    CHECK(test::savedDigest(painter, {.value = 7}, resources) == live);
    CHECK(test::savedDigest(painter, {.value = 8}, resources) != live);
}

} // namespace
