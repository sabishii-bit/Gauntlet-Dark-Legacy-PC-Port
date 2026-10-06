#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/BossDefinition.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/CombatantAssets.h"
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("combatant bitmap skin runs use the native float count and reject incomplete runs",
          "[combatant-bindings][asset-conformance]") {
    const auto root = test::scratchDirectory("combatant-bitmap-skins");
    const auto archive = root / "MONSTERS/DRAGON";
    std::filesystem::create_directories(archive);
    std::filesystem::create_directories(root / "critter");
    writeTextFile(archive / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
      {"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    auto bitmaps = nlohmann::json::array();
    constexpr u32 kFrames = 21;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        bitmaps.push_back({{"name", "FADE" + std::to_string(frame)},
                           {"file", "skin.png"},
                           {"width", 2},
                           {"height", 2}});
    }
    writeTextFile(archive / "textures.json", nlohmann::json{{"bitmaps", bitmaps}}.dump());
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"READY","frames":2,"frameRate":30}]}]})");
    // Single-precision multiplication gives 21, while widening 0.7f first would
    // truncate to 20. The loader and the draw clock must agree on the frame count.
    f32 life = 0.7f;
    usize expected = kFrames;
    SECTION("complete subsecond run") {}
    SECTION("incomplete run is not partially cached") {
        life = 1;
        expected = 0;
    }
    SECTION("zero duration") {
        life = 0;
        expected = 0;
    }
    SECTION("negative duration") {
        life = -1;
        expected = 0;
    }
    auto data = nlohmann::json::parse(R"({"name":"DRAGON",
      "descriptors":[{"name":"dragon","prefix":"BODY","type":4}],
      "types":[{"maxHealth":10,"moveBase":0,"moveCount":1}],
      "moves":[{"name":"READY","anim":"READY","type":32}],
      "sounds":[{"name":"FADE0","flags":256,"rate":1}]})");
    data["sounds"][0]["life"] = life;
    writeTextFile(root / "critter/DRAGON.json", data.dump());
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("DRAGON"), 'B'));
    if (expected == 0) {
        CHECK_FALSE(assets.skins.contains("FADE0"));
    } else {
        REQUIRE(assets.skins.contains("FADE0"));
        CHECK(assets.skins.at("FADE0").size() == expected);
    }
}

TEST_CASE("catalogued boss effects bind as skins particles or trees in the level's actual context",
          "[native-assets][combatant-bindings][assets]") {
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    AnimationSet shared;
    REQUIRE(shared.load(root / "WEAPONS"));
    SoundSet common;
    SoundSet ambient;
    REQUIRE(common.load(root / "AUDIO/COMMON"));
    REQUIRE(ambient.load(root / "AUDIO/TOWAMB"));
    usize encounters = 0;
    usize effects = 0;
    std::set<std::string> missingSounds;
    for (const auto& realm : catalog.realms()) {
        WorldData world;
        REQUIRE(world.load(root / "WDATA" / (realm.file + ".WAD")));
        for (const auto& level : world.levels()) {
            const auto name = bossNameOf(level.bossType);
            if (name.empty()) {
                continue;
            }
            CAPTURE(level.name, name);
            const auto ref = catalog.byName(level.name);
            REQUIRE(ref);
            const auto* audio = world.audio(level.audioIndex);
            REQUIRE(audio != nullptr);
            SoundSet bank;
            REQUIRE(bank.load(root / "AUDIO" / audio->bank));
            test::FakeRenderDevice device;
            TextureSet stage;
            REQUIRE(stage.load(root / ref->directory));
            const std::array lenders{&stage};
            CombatantAssets assets;
            REQUIRE(assets.load(device, root, bossDefinition(name), level.name.front(), lenders));
            for (usize i = 0; i < assets.data.sounds().size(); ++i) {
                const auto& effect = assets.data.sounds()[i];
                CAPTURE(i, effect.tree, effect.flags, effect.soundFormat);
                if (effect.shows()) {
                    constexpr u32 kParticle = 0x0F000000;
                    if ((effect.flags & kParticle) != 0) {
                        // CritterDoParticle resolves a bitmap, not an animation tree.
                        // The shipped Dragon link is also exercised by projectile-trail tests.
                        REQUIRE(assets.archive.textures.find(effect.tree));
                    } else if ((effect.flags & CombatEffectDefinition::kSkin) != 0) {
                        REQUIRE(assets.skins.contains(effect.tree));
                        REQUIRE_FALSE(assets.skins.at(effect.tree).empty());
                    } else {
                        REQUIRE(
                            (assets.archive.trees.find(effect.tree) || shared.find(effect.tree)));
                    }
                }
                const auto sound = effect.soundFor(level.name.front());
                if (!sound.empty() && !bank.find(sound) && !common.find(sound) &&
                    !ambient.find(sound)) {
                    missingSounds.insert(std::string(name) + ":" + sound);
                }
                ++effects;
            }
            ++encounters;
        }
    }
    CHECK(encounters == 11);
    CHECK(effects > 300);
    // A shipped Spider SFXX names this Lich sound, which none of its loaded banks
    // supplies. AudioFindSound's failure leaves it silent. Keep the exact gap as a
    // review, not a global search that imports audio from an unrelated encounter.
    CHECK(missingSounds == std::set<std::string>{"DRIDER:S_LICHMEAT"});
}

TEST_CASE("all native boss move animations and collision anchors bind in their actual body tree",
          "[native-assets][combatant-bindings][assets]") {
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    usize moves = 0;
    usize shortenedNodes = 0;
    for (s32 kind = 34; kind <= 44; ++kind) {
        const auto name = bossNameOf(kind);
        CAPTURE(name);
        test::FakeRenderDevice device;
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, bossDefinition(name), 'K'));
        const auto check = [&](const CritterData& data) {
            for (const auto& move : data.moves()) {
                CAPTURE(move.name, move.anim, move.colnode);
                REQUIRE(assets.tree->findSequence(move.anim));
                if (!move.colnode.empty()) {
                    REQUIRE(assets.tree->findNode(move.colnode, kCombatantNodeNameLength));
                    if (!assets.tree->findNode(move.colnode)) {
                        ++shortenedNodes;
                    }
                }
                ++moves;
            }
        };
        check(assets.data);
        for (const auto& child : assets.children) {
            check(child);
        }
    }
    CHECK(moves > 300);
    CHECK(shortenedNodes == 8);
}

TEST_CASE("Plague Fiend abbreviated tentacle anchors use the animated node instead of body origin",
          "[native-assets][combatant-bindings][assets]") {
    const auto root = test::assetOrSkip("CRITTER/PBOSS.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("PBOSS"), 'K'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {11, 7, 19}, 0.7f, nullptr, {}, 'K'));
    constexpr std::string_view kReference = "BODY1_TENT_COLLI";
    constexpr std::string_view kFullName = "BODY1_TENT_COLLISION";
    REQUIRE(assets.tree->findNode(kReference, kCombatantNodeNameLength) ==
            assets.tree->findNode(kFullName));
    EnemyView player;
    player.position = {11, 7, 45};
    player.player = 0;
    player.radius = 1;
    player.height = 6;
    const std::array players{player};
    bool awayFromRoot = false;
    for (s32 frame = 0; frame < 300; ++frame) {
        actor.update(2, 1.0f / 30, players);
        const auto full = actor.nodeTransform(kFullName);
        const auto shortName = actor.nodeTransform(kReference);
        const auto rootTransform = actor.rootTransform();
        REQUIRE(full);
        REQUIRE(shortName);
        REQUIRE(rootTransform);
        CHECK(*shortName == *full);
        awayFromRoot |= Vec3{(*shortName)[3]} != Vec3{(*rootTransform)[3]};
    }
    CHECK(awayFromRoot);
}

TEST_CASE(
    "native Dragon and Yeti death skins resolve bitmap runs without a texture animation record",
    "[native-assets][combatant-bindings][assets]") {
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    struct Expected {
        const char* name;
        const char* level;
        const char* skin;
        usize frames;
        bool borrowed;
    };
    for (const auto& expected : {Expected{"DRAGON", "B6", "LAVA00", 30, true},
                                 Expected{"YETI", "I5", "SEETHROUGH_03", 1, false}}) {
        CAPTURE(expected.name);
        const char realm = std::string_view(expected.level).front();
        test::FakeRenderDevice device;
        TextureSet stage;
        REQUIRE(stage.load(root / "LEVELS" / (std::string("LEVEL") + expected.level)));
        const std::array lenders{&stage};
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, bossDefinition(expected.name), realm, lenders));
        REQUIRE(assets.skins.contains(expected.skin));
        const auto& frames = assets.skins.at(expected.skin);
        REQUIRE(frames.size() == expected.frames);
        auto& owner = expected.borrowed ? stage : assets.archive.textures;
        const auto first = owner.find(expected.skin);
        REQUIRE(first);
        if (expected.borrowed) {
            CHECK_FALSE(assets.archive.textures.find(expected.skin));
        } else {
            CHECK(owner.entry(*first).frames == 0);
        }
        for (usize frame = 0; frame < frames.size(); ++frame) {
            CHECK(frames[frame] == &owner.texture(device, *first + static_cast<u32>(frame)));
        }
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {0, 0, 0}, 0, nullptr, {}, realm));
        EnemyHit hit;
        hit.damage = 1000000;
        REQUIRE(actor.hurt(hit) > 0);
        bool seen = false;
        for (s32 frame = 0; frame < 180 && actor.present(); ++frame) {
            actor.update(2, 1.0f / 30, {});
            device.draws.clear();
            actor.draw(device, Mat4{1}, {});
            for (const auto& draw : device.draws) {
                if (draw.state.maskedTexture != nullptr) {
                    CHECK(std::ranges::find(frames, draw.state.maskedTexture) != frames.end());
                    seen = true;
                }
            }
        }
        CHECK(seen);
        // Another living instance must not inherit its predecessor's death appearance.
        REQUIRE(actor.spawn(assets, 0, {0, 0, 0}, 0, nullptr, {}, realm));
        device.draws.clear();
        actor.draw(device, Mat4{1}, {});
        REQUIRE_FALSE(device.draws.empty());
        for (const auto& draw : device.draws) {
            CHECK(draw.state.maskedTexture == nullptr);
        }
    }
}
} // namespace
