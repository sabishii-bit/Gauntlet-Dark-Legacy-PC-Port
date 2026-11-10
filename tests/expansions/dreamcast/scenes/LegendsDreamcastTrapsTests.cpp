#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SelectionHighlight.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/level/LegendsDreamcastTraps.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/combat/DamageTypes.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "levels/levela1/worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    return root;
}

TEST_CASE("Dreamcast B1 binds every authored flame to an animated damaging fixture",
          "[legends][dreamcast-traps][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    const auto directory = root / "levels/levelb1";
    if (!std::filesystem::exists(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    const auto native = formats::LegendsDreamcastScene::parse(bytes);
    usize flameCount = 0;
    std::vector<LegendsPortalVisitor> visitors;
    std::vector<u32> nativeIndices;
    for (usize i = 0; i < native.itemInstances.size(); ++i) {
        const auto& item = native.itemInstances[i];
        const auto& info = native.itemInfos[static_cast<usize>(item.info)];
        if (info.type != 8) {
            continue;
        }
        std::string warning;
        INFO(i << ": " << info.name);
        REQUIRE(
            LegendsDreamcastTrap::decode(info, item, static_cast<u32>(i), {1, 4, 6, 4}, &warning));
        ++flameCount;
        visitors.push_back({.player = 0, .position = item.position});
        nativeIndices.push_back(static_cast<u32>(i));
    }
    REQUIRE(flameCount == 20);
    test::FakeRenderDevice device;
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 4);
    REQUIRE(scene);
    std::set<u32> hitTraps;
    for (usize index = 0; index < visitors.size(); ++index) {
        // Native fxhittime belongs to the player, not each trap. Visit each
        // placement in turn rather than posing one player in twenty places.
        scene->updateTraps(3, true);
        for (s32 tick = 0; tick < 240 && !hitTraps.contains(nativeIndices[index]); ++tick) {
            scene->updateTraps(1.f / 60, false);
            CHECK(scene->takeSounds().empty()); // AudioTrap belongs to contact, not animation.
            scene->touchTraps(std::span{&visitors[index], 1});
            const auto hits = scene->takeTrapHits();
            const auto sounds = scene->takeSounds();
            CHECK(sounds.size() == hits.size());
            for (const auto& sound : sounds) {
                CHECK(sound.token == 0x210027);
            }
            for (const auto& value : hits) {
                hitTraps.insert(value.nativeIndex);
                CHECK(value.damage == 20);
            }
        }
        CHECK(hitTraps.contains(nativeIndices[index]));
    }
    for (const auto& warning : scene->warnings()) {
        INFO(warning);
    }
    CHECK(hitTraps.size() == flameCount);
    scene->updateTraps(1.f / 60, true);
    scene->touchTraps(visitors);
    CHECK(scene->takeTrapHits().empty());
    CHECK(scene->takeSounds().empty());
}

TEST_CASE("Dreamcast Castle binds all spikes to visible cycles and physical contacts",
          "[legends][dreamcast-traps][dreamcast-spikes][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/levela1";
    const auto worlds = readFile(directory / "worlds.rom");
    const auto native = formats::LegendsDreamcastScene::parse(worlds);
    auto map = formats::LegendsMap::parseDreamcast(worlds, readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 4);
    REQUIRE(scene);
    std::vector<u32> indices;
    for (usize index = 0; index < native.itemInstances.size(); ++index) {
        const auto& info = native.itemInfos[static_cast<usize>(native.itemInstances[index].info)];
        if (info.type == 8 && info.subtype == 0) {
            indices.push_back(static_cast<u32>(index));
        }
    }
    REQUIRE(indices.size() == 17);
    s32 selected = -1;
    for (usize index = 0; index < scene->fixtureCount(); ++index) {
        const s32 id = LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(index);
        const auto object = scene->fixtureObject(id);
        if (object && object->name == "SPIKES" &&
            glm::length(Vec2{object->position.x - native.itemInstances[indices.front()].position.x,
                             object->position.z -
                                 native.itemInstances[indices.front()].position.z}) < .001f) {
            selected = id;
            break;
        }
    }
    REQUIRE(selected >= 0);
    const auto geometry = [&] {
        device.draws.clear();
        scene->highlightFixture(device, Mat4{1}, CameraFrame{}, 1, selected,
                                SelectionHighlight{Vec3{0, 100, 0}});
        std::vector<Vec3> vertices;
        for (const auto& draw : device.draws) {
            for (const auto& vertex : draw.vertices) {
                vertices.emplace_back(draw.transform * Vec4{vertex.position, 1});
            }
        }
        return vertices;
    };
    const auto resting = geometry();
    REQUIRE_FALSE(resting.empty());
    for (const auto index : indices) {
        INFO(index);
        scene->updateTraps(3, true);
        const std::array visitors{
            LegendsPortalVisitor{.position = native.itemInstances[index].position}};
        bool hit = false;
        for (s32 tick = 0; tick < 240 && !hit; ++tick) {
            scene->capturePresentation();
            scene->updateTraps(1.f / 60, false);
            CHECK(scene->takeSounds().empty());
            scene->touchTraps(visitors);
            const auto contacts = scene->takeTrapHits();
            const auto sounds = scene->takeSounds();
            CHECK(sounds.size() == contacts.size());
            for (const auto& sound : sounds) {
                CHECK(sound.token == 0x23000f);
            }
            for (const auto& contact : contacts) {
                CHECK(contact.nativeIndex == index);
                CHECK(contact.damage == 20);
                CHECK(contact.pierces);
                CHECK(contact.impact.flags == (damage::kSpike | damage::kStun | damage::kLow));
                hit = hit || contact.nativeIndex == index;
            }
        }
        REQUIRE(hit);
        if (index == indices.front()) {
            for (s32 tick = 0; tick < 12; ++tick) {
                scene->capturePresentation();
                scene->updateTraps(1.f / 60, false);
            }
            const auto striking = geometry();
            REQUIRE_FALSE(striking.empty());
            CHECK(striking != resting);
            for (s32 tick = 0; tick < 72; ++tick) {
                scene->capturePresentation();
                scene->updateTraps(1.f / 60, true);
                scene->touchTraps(visitors);
                CHECK(scene->takeTrapHits().empty());
            }
            // Empty OFF must retain the completed animation, not freeze an
            // interrupted ON pose with spikes still sticking out of the floor.
            const auto stopped = geometry();
            REQUIRE_FALSE(stopped.empty());
            CHECK(stopped != striking);
        }
    }
    for (const auto& warning : scene->warnings()) {
        CHECK(warning.find("SPIKES") == std::string::npos);
    }
}

TEST_CASE("Dreamcast overlapping spike contacts share each player's native hit cooldown",
          "[legends][dreamcast-traps][dreamcast-spikes][assets]") {
    const auto directory = nativeRoot() / "levels/levela1";
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"),
                                                   readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 4);
    REQUIRE(scene);
    // Deliberately overlap all spike contacts with one broad visitor volume.
    // The regression is a shared hit gate, not the dimensions of a character.
    std::array<LegendsPortalVisitor, 4> visitors;
    for (u8 player = 0; player < 4; ++player) {
        visitors[player].player = player;
        visitors[player].radius = 1000;
        visitors[player].height = 1000;
    }
    bool struck = false;
    for (s32 tick = 0; tick < 240 && !struck; ++tick) {
        scene->updateTraps(1.f / 60, false);
        auto levitating = visitors;
        for (auto& visitor : levitating) {
            visitor.levitating = true;
        }
        scene->touchTraps(levitating);
        CHECK(scene->takeTrapHits().empty());
        CHECK(scene->takeSounds().empty());
        // The rejected levitating contacts must not consume the hit gate.
        scene->touchTraps(visitors);
        const auto hits = scene->takeTrapHits();
        if (hits.empty()) {
            continue;
        }
        REQUIRE(hits.size() == 4);
        for (u8 player = 0; player < 4; ++player) {
            CHECK(std::ranges::count(hits, player, &LegendsTrapHit::player) == 1);
        }
        scene->takeSounds();
        // Other traps enter ON during this window, but cannot bypass fxhittime.
        for (s32 held = 0; held < 30; ++held) {
            scene->updateTraps(1.f / 60, false);
            scene->touchTraps(visitors);
            CHECK(scene->takeTrapHits().empty());
            CHECK(scene->takeSounds().empty());
        }
        struck = true;
    }
    REQUIRE(struck);
}
} // namespace
