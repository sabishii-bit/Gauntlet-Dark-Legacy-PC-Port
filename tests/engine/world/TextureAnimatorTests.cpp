#include <array>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"

namespace {

using namespace gdl;
using Catch::Approx;

TextureAnimationInfo cycle(std::string_view name, s32 texture, s32 source, s32 frames, s32 start,
                           s32 rate, std::string_view frameName = "") {
    TextureAnimationInfo info;
    info.name = std::string(name);
    info.frameName = std::string(frameName);
    info.texture = texture;
    info.source = source;
    info.frames = frames;
    info.start = start;
    info.rate = rate;
    return info;
}

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    TextureSet lender;
    WorldLayout layout;
    WorldScene scene;
    TextureAnimator animator;
    std::array<TextureSet*, 1> lenders{&lender};

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
        REQUIRE(lender.load(test::sampleLender(std::string(name) + "-lender")));
        REQUIRE(scene.build(layout, models, textures, device, WorldLighting{}, lenders));
    }
};

TEST_CASE("texture animations cycle frames and slide coordinates once a game frame",
          "[world][animation]") {
    Fixture f("texture-animator");
    const std::vector<TextureAnimationInfo> animations{
        // The torch shows the lender's two flame frames every second game frame, from the
        // second; the stone shows the set's next two entries; the glass scrolls back along
        // u over four steps.
        cycle("TORCHB", 3, TextureAnimationInfo::kByName, 2, 1, 2, "TORCH00"),
        cycle("STONE", 0, 1, 2, 0, 1),
        cycle("GLASS", 1, TextureAnimationInfo::kScrollU, -4, 0, 0),
        cycle("LOST", 0, TextureAnimationInfo::kByName, 3, 0, 1, "NOPE"),
        cycle("NOWHERE", -1, 1, 2, 0, 1),
        cycle("STILL", 0, 1, 0, 0, 1),
    };
    f.animator.bind(animations, f.textures, f.device, f.lenders);
    REQUIRE(f.animator.size() == 3);
    REQUIRE(f.animator.slot(0) == 3);
    REQUIRE(f.animator.counter(0) == 1);
    REQUIRE(f.animator.frame() == 0);

    f.animator.apply(f.scene);
    REQUIRE(f.scene.textureOf(3) == &f.lender.texture(f.device, 2));
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 1));
    REQUIRE(f.scene.textureOffset(1) == Vec2{0.0f, 0.0f});

    f.animator.step(f.scene);
    REQUIRE(f.animator.frame() == 1);
    REQUIRE(f.scene.textureOf(3) == &f.lender.texture(f.device, 2)); // not its frame yet
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 2));
    REQUIRE(f.scene.textureOffset(1) == Vec2{-0.25f, 0.0f});

    f.animator.step(f.scene);
    REQUIRE(f.scene.textureOf(3) == &f.lender.texture(f.device, 1));
    REQUIRE(f.animator.counter(0) == 0);
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 1));
    REQUIRE(f.scene.textureOffset(1) == Vec2{-0.5f, 0.0f});
    // The same stands can be read as motions, and stepped without a scene.
    REQUIRE(f.animator.motion(0).slot == 3);
    REQUIRE(f.animator.motion(0).frame == &f.lender.texture(f.device, 1));
    REQUIRE(f.animator.motion(0).offset == Vec2{0.0f, 0.0f});
    bool scrolled = false;
    for (usize i = 0; i < f.animator.size(); ++i) {
        const TextureMotion motion = f.animator.motion(i);
        if (motion.slot == 1) {
            scrolled = true;
            REQUIRE(motion.frame == nullptr);
            REQUIRE(motion.offset == Vec2{-0.5f, 0.0f});
        }
    }
    REQUIRE(scrolled);

    f.animator.step(f.scene, 2);
    REQUIRE(f.animator.frame() == 4);
    REQUIRE(f.scene.textureOffset(1) == Vec2{0.0f, 0.0f});
    REQUIRE(f.scene.textureOf(3) == &f.lender.texture(f.device, 2));

    // Without a scene the animations still move on, to be shown elsewhere.
    f.animator.step(2);
    REQUIRE(f.animator.frame() == 6);
    REQUIRE(f.animator.motion(0).frame == &f.lender.texture(f.device, 1));
    f.animator.clear();
    REQUIRE(f.animator.size() == 0);
    REQUIRE(f.animator.frame() == 0);
}

TEST_CASE("an animation keyed to a sequence is never stepped, but read off at a frame",
          "[world][animation]") {
    Fixture f("texture-animator");
    // A stone cycle of two frames from the sequence's fourth frame, a frame each two; and a
    // glass scroll that eases in over four frames from the sixth and runs eight.
    TextureAnimationInfo keyedCycle = cycle("STONE", 0, 1, 2, 0, 2);
    keyedCycle.flag = 2;
    keyedCycle.offset = 4;
    TextureAnimationInfo keyedScroll = cycle("GLASS", 1, TextureAnimationInfo::kScrollU, 8, 0, 4);
    keyedScroll.flag = 7;
    keyedScroll.offset = 6;
    const std::vector<TextureAnimationInfo> animations{
        cycle("TORCHB", 3, TextureAnimationInfo::kByName, 2, 1, 2, "TORCH00"), keyedCycle,
        keyedScroll};
    f.animator.bind(animations, f.textures, f.device, f.lenders);
    REQUIRE(f.animator.size() == 3);
    REQUIRE_FALSE(f.animator.keyed(0));
    REQUIRE(f.animator.keyed(1));
    REQUIRE(f.animator.keyed(2));
    // Stepping and applying leave the keyed ones alone.
    const Texture* own = f.scene.textureOf(0);
    f.animator.step(f.scene, 4);
    f.animator.apply(f.scene);
    REQUIRE(f.animator.counter(1) == 0);
    REQUIRE(f.scene.textureOf(0) == own);
    REQUIRE(f.scene.textureOffset(1) == Vec2{0.0f, 0.0f});
    // Before its first frame the cycle shows its first, then a frame each two, and holds
    // its last.
    REQUIRE(f.animator.motionAt(1, 0)->frame == &f.textures.texture(f.device, 1));
    REQUIRE(f.animator.motionAt(1, 4)->frame == &f.textures.texture(f.device, 1));
    REQUIRE(f.animator.motionAt(1, 6)->frame == &f.textures.texture(f.device, 2));
    REQUIRE(f.animator.motionAt(1, 40)->frame == &f.textures.texture(f.device, 2));
    REQUIRE(f.animator.motionAt(1, 6)->slot == 0);
    // The scroll: still before its frame, easing over its rate, steady to its end, then held.
    REQUIRE(f.animator.motionAt(2, 6)->offset == Vec2{0.0f, 0.0f});
    REQUIRE(f.animator.motionAt(2, 6)->frame == nullptr);
    REQUIRE(f.animator.motionAt(2, 8)->offset.x < 0.0f);
    REQUIRE(f.animator.motionAt(2, 8)->offset.y == 0.0f);
    REQUIRE(f.animator.motionAt(2, 6 + 10)->offset.x == Approx(0.5f));
    REQUIRE(f.animator.motionAt(2, 6 + 10)->offset.x == TextureAnimator::scrollAt(10, 4, 8));
    REQUIRE(f.animator.motionAt(2, 100)->offset.x == 1.0f);
    // Nothing for what was not bound.
    REQUIRE_FALSE(f.animator.motionAt(-1, 3).has_value());
    REQUIRE_FALSE(f.animator.motionAt(9, 3).has_value());
    // Without a rate the scroll runs a step a frame round its cycle.
    REQUIRE(TextureAnimator::scrollAt(3, 0, 8) == Approx(3.0f / 8.0f));
    REQUIRE(TextureAnimator::scrollAt(11, 0, 8) == Approx(3.0f / 8.0f));
    // The stretch it puts on the coordinate it slides: nothing of the picture before it
    // starts, growing as it eases in, twice (its frames over its rate) from its end, and
    // always what it reaches less the slide; the other coordinate is left alone.
    REQUIRE(TextureAnimator::scrollStateAt(0, 4, 8).scale == 0.0f);
    REQUIRE(f.animator.motionAt(2, 6)->scale == Vec2{0.0f, 1.0f});
    REQUIRE(f.animator.motionAt(2, 0)->scale == Vec2{0.0f, 1.0f});
    const ScrollState easing = TextureAnimator::scrollStateAt(2, 4, 8);
    REQUIRE(easing.scale == Approx(1.0f - easing.along));
    REQUIRE(easing.scale > 1.0f);
    const ScrollState steady = TextureAnimator::scrollStateAt(6, 4, 8);
    REQUIRE(steady.scale == Approx(1.5f - steady.along));
    REQUIRE(TextureAnimator::scrollStateAt(9, 4, 8).scale == Approx(2.0f - 0.25f));
    REQUIRE(TextureAnimator::scrollStateAt(100, 4, 8).scale == Approx(1.0f));
    REQUIRE(f.animator.motionAt(2, 100)->scale == Vec2{1.0f, 1.0f});
    REQUIRE(TextureAnimator::scrollStateAt(3, 0, 8).scale == 0.0f);
    // A rate past the frames holds still, stretched by one.
    REQUIRE(TextureAnimator::scrollStateAt(10, 9, 8).along == 0.0f);
    REQUIRE(TextureAnimator::scrollStateAt(10, 9, 8).scale == 1.0f);
}

TEST_CASE("a cycle ends where its frames cannot be read", "[world][animation]") {
    Fixture f("texture-animator-short");
    const std::vector<TextureAnimationInfo> animations{cycle("STONE", 0, 1, 5, 0, 1)};
    f.animator.bind(animations, f.textures, f.device, f.lenders);
    REQUIRE(f.animator.size() == 1);
    // Entries 1 and 2 follow the source and the external 3 has no image, so the cycle is
    // two long.
    f.animator.step(f.scene, 2);
    REQUIRE(f.animator.counter(0) == 0);
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 1));
    f.animator.step(f.scene);
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 2));
}

} // namespace
