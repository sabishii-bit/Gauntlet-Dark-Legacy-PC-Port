#include <algorithm>
#include <array>
#include <format>
#include <unordered_set>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/PlayerFamiliar.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("permanent familiar tiers are awarded at thirty and eighty", "[game][familiar]") {
    REQUIRE(PlayerFamiliar::tierFor(1) == 0);
    REQUIRE(PlayerFamiliar::tierFor(29) == 0);
    REQUIRE(PlayerFamiliar::tierFor(30) == 1);
    REQUIRE(PlayerFamiliar::tierFor(79) == 1);
    REQUIRE(PlayerFamiliar::tierFor(80) == 2);
    REQUIRE(PlayerFamiliar::tierFor(99) == 2);
    test::FakeRenderDevice device;
    PlayerFamiliar familiar;
    ItemArchive empty;
    REQUIRE_FALSE(familiar.bind(device, empty, 30, Vec3{0}));
    REQUIRE(familiar.tier() == 0);
    familiar.update(1, true);
    familiar.draw(device, Mat4{1}, Mat4{1}, WorldLighting{}, 1);
    REQUIRE_FALSE(familiar.bound());
}

TEST_CASE("both permanent familiars bind the authored animated assets",
          "[game][familiar][assets]") {
    const auto path = test::assetOrSkip("PLAYERS/WAR/SFXYEL/ANIM.PS2").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(path));
    PlayerFamiliar familiar;
    for (const s32 level : {30, 80}) {
        REQUIRE(familiar.bind(device, archive, level, Vec3{1, 5, -1}));
        REQUIRE(familiar.tier() == PlayerFamiliar::tierFor(level));
        for (s32 frame = 0; frame < 60; ++frame) {
            familiar.update(1.0f / 30.0f, frame == 20);
            familiar.draw(device, Mat4{1}, Mat4{1}, WorldLighting{}, 1);
        }
    }
    REQUIRE_FALSE(familiar.bind(device, archive, 29, Vec3{0}));
    REQUIRE(familiar.tier() == 0);
}

TEST_CASE("every class and color familiar resolves shared animated frame pixels",
          "[game][familiar][assets][asset-coverage]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    TextureSet shared;
    REQUIRE(shared.load(root / "WEAPONS"));
    test::FakeRenderDevice device;
    const std::array<TextureSet*, 1> lenders{&shared};
    usize visited = 0;
    usize sharedCycles = 0;
    for (const auto* cls : {"WAR", "VAL", "WIZ", "ARC", "DWF", "KNI", "SOR", "JES"}) {
        for (const auto* color : {"YEL", "BLU", "RED", "GRE"}) {
            CAPTURE(cls, color);
            ItemArchive archive;
            REQUIRE(archive.load(root / "PLAYERS" / cls / std::format("SFX{}", color)));
            TextureAnimator animations;
            animations.bind(archive.trees.textureAnimations(), archive.textures, device, lenders);
            const auto& records = archive.trees.textureAnimations();
            for (usize index = 0; index < records.size(); ++index) {
                const auto& record = records[index];
                if (record.texture < 0 || record.frames == 0 || !record.cycles()) {
                    continue;
                }
                CAPTURE(record.name, record.frameName);
                REQUIRE(animations.motionAt(static_cast<s32>(index), 0).has_value());
                if (record.source < 0) {
                    ++sharedCycles;
                }
            }
            for (const s32 level : {30, 80}) {
                PlayerFamiliar familiar;
                REQUIRE(familiar.bind(device, archive, level, Vec3{0}, lenders));
                const auto treeIndex =
                    archive.trees.find(std::format("FAMILIAR{}", PlayerFamiliar::tierFor(level)));
                REQUIRE(treeIndex.has_value());
                std::unordered_set<u32> usedSlots;
                for (const auto& node : archive.trees.tree(*treeIndex).nodes) {
                    if (const auto object = archive.models.find(node.object)) {
                        for (const auto& part : archive.models.mesh(*object).parts) {
                            usedSlots.insert(part.texture);
                        }
                    }
                }
                std::unordered_set<const Texture*> shown;
                for (s32 frame = 0; frame < 30; ++frame) {
                    device.draws.clear();
                    familiar.update(1.0f / 30.0f, false);
                    familiar.draw(device, Mat4{1}, Mat4{1}, {}, 1);
                    REQUIRE_FALSE(device.draws.empty());
                    for (const auto& draw : device.draws) {
                        REQUIRE(draw.texture != nullptr);
                        // An unresolved noPicture slot returns a transparent 1x1 tile.
                        CHECK(draw.texture->width() > 1);
                        CHECK(draw.texture->height() > 1);
                        shown.insert(draw.texture);
                    }
                }
                REQUIRE_FALSE(shown.empty());
                for (usize index = 0; index < records.size(); ++index) {
                    const auto& record = records[index];
                    if (!record.cycles() || !record.freeRunning() || record.frames < 2 ||
                        !usedSlots.contains(static_cast<u32>(record.texture))) {
                        continue;
                    }
                    CAPTURE(record.name, level);
                    const auto first = animations.motionAt(static_cast<s32>(index), 0);
                    const auto second =
                        animations.motionAt(static_cast<s32>(index), std::max(record.rate, 1));
                    REQUIRE(first.has_value());
                    REQUIRE(second.has_value());
                    REQUIRE(first->frame != second->frame);
                    CHECK(shown.contains(first->frame));
                    CHECK(shown.contains(second->frame));
                }
                ++visited;
            }
        }
    }
    REQUIRE(visited == 64);
    REQUIRE(sharedCycles > 0);
}
} // namespace
