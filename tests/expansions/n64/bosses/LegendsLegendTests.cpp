#include <algorithm>
#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "expansions/n64/bosses/LegendsLegend.h"
#include "expansions/n64/formats/LegendsN64Critter.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/bosses/BossSequence.h"
#include "game/bosses/LegendPresentation.h"
#include "game/critters/Combatant.h"
#include "game/effects/EffectObservation.h"
#include "game/enemies/ScriptedOpponents.h"

namespace {
using namespace gdl;
using namespace gdl::game;
std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("Native N64 assets unavailable");
    }
    return root;
}
} // namespace

TEST_CASE("All four native legendary weapons render in the hand and fly using their own art",
          "[legends][native-legends][assets]") {
    const auto root = nativeRoot();
    const auto program = readFile(root / "_program/game.bin");
    constexpr std::array kKinds{34, 39, 35, 38};
    constexpr std::array kHeld{"POWERUP:QICE", "POWERUP:QFLM", "POWERUP:QSCM", "POWERUP:QJAV"};
    for (s32 i = 0; i < 4; ++i) {
        CAPTURE(i);
        std::vector<std::string> warnings;
        auto binding = loadLegendsLegend(root, i, program, warnings);
        REQUIRE(binding.archive);
        REQUIRE(binding.archive->loaded());
        CHECK(binding.profile.held == kHeld[static_cast<usize>(i)]);
        REQUIRE(binding.archive->trees.find(binding.profile.held));
        REQUIRE(binding.archive->trees.find(binding.profile.projectile));
        test::FakeRenderDevice device;
        ItemArchive weapons;
        TextureSet shared;
        EffectTrees effects;
        std::vector<LegendSound> sounds;
        usize donorSounds = 0;
        LegendPresentation presentation(
            effects, {device, *binding.archive, weapons, shared, &binding.profile},
            {.play =
                 [&](std::string_view) {
                     ++donorSounds;
                     return kNoSound;
                 },
             .mapped =
                 [&](LegendSound phase) {
                     sounds.push_back(phase);
                     return true;
                 }});
        LegendPresentation::Bearer bearer;
        bearer.player = 0;
        bearer.position = {0, 0, 0};
        bearer.facing = {0, 0, 1};
        bearer.holdPoint = {1, 3, 0};
        bearer.canGesture = true;
        const LegendPresentation::Target target{Vec3{0, 0, 12}, 6};
        presentation.carry(0, legendRealmOf(kKinds[static_cast<usize>(i)]),
                           kKinds[static_cast<usize>(i)], bearer);
        REQUIRE(effects.count() == 1);
        CHECK(effects.effect(0).name == binding.profile.held);
        CHECK(presentation.occupiedHand() == 0);
        const auto camera = CameraFrame::of(WorldCamera{});
        effects.draw(device, Mat4{1}, {}, &camera);
        REQUIRE_FALSE(device.draws.empty());
        presentation.show(LegendCue::Brandished, 0, legendRealmOf(kKinds[static_cast<usize>(i)]),
                          kKinds[static_cast<usize>(i)], bearer);
        presentation.show(LegendCue::Thrown, 0, legendRealmOf(kKinds[static_cast<usize>(i)]),
                          kKinds[static_cast<usize>(i)], bearer);
        bearer.released = true;
        CHECK_FALSE(presentation.update(0, bearer, target).landed);
        REQUIRE(effects.count() >= 1);
        const auto* flying = [&]() -> const EffectTrees::Effect* {
            for (usize j = 0; j < effects.count(); ++j) {
                if (effects.effect(j).name == binding.profile.projectile) {
                    return &effects.effect(j);
                }
            }
            return nullptr;
        }();
        REQUIRE(flying);
        CHECK(flying->position == Vec3{0, 2, 1}); // no GameCube Yeti +85 arena offset
        bool landed = false;
        for (s32 tick = 0; tick < 180 && !landed; ++tick) {
            effects.update(1.f / 60);
            landed = presentation.update(1.f / 60, bearer, target).landed;
        }
        CHECK(landed);
        for (s32 tick = 0; tick < 180; ++tick) {
            effects.update(1.f / 60);
            CHECK_FALSE(presentation.update(1.f / 60, bearer, target).landed);
        }
        CHECK(donorSounds == 0);
        CHECK(sounds == std::vector{LegendSound::PickedUp, LegendSound::Thrown, LegendSound::Flying,
                                    LegendSound::Landed});
    }
    std::vector<std::string> warnings;
    CHECK_FALSE(loadLegendsLegend({}, 4, {}, warnings).archive);
}

TEST_CASE("Native legendary audio maps authored one-shots without fabricating flight loops",
          "[legends][native-legends][native-legend-audio]") {
    for (s32 index = 0; index < 4; ++index) {
        CAPTURE(index);
        CHECK(legendsLegendSound(index, LegendSound::PickedUp) == 0x9e);
        CHECK(legendsLegendSound(index, LegendSound::Thrown) == (index == 1 ? 0x122 : 0x39));
        CHECK_FALSE(legendsLegendSound(index, LegendSound::Flying));
        CHECK_FALSE(legendsLegendSound(index, LegendSound::WornOff));
    }
    CHECK(legendsLegendSound(0, LegendSound::Landed) == 0x3e);
    CHECK(legendsLegendSound(1, LegendSound::Landed) == 0x93);
    CHECK_FALSE(legendsLegendSound(2, LegendSound::Landed));
    CHECK(legendsLegendSound(3, LegendSound::Landed) == 0x74);
    CHECK_FALSE(legendsLegendSound(-1, LegendSound::PickedUp));
    CHECK_FALSE(legendsLegendSound(4, LegendSound::PickedUp));
}

TEST_CASE("Native relics target the bound lion and eye without borrowing absent GDL body art",
          "[legends][native-legends][legend-profile][assets]") {
    const auto native = nativeRoot();
    const auto common = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(native / "_program/game.bin");
    for (const s32 index : {2, 3}) {
        CAPTURE(index);
        const s32 kind = index == 2 ? 35 : 38;
        test::FakeRenderDevice device;
        std::vector<std::string> warnings;
        auto legend = loadLegendsLegend(native, index, program, warnings);
        REQUIRE(legend.profile.targetNode);
        const auto names = formats::decodeLegendsN64BoneNames(
            readFile(native / (index == 2 ? "chimera/seq.wad" : "slime/seq.wad")));
        CHECK(legend.profile.targetNode ==
              formats::findLegendsN64Bone(names, index == 2 ? "BODY1_LION" : "BODY1_EYEBALL"));
        CHECK(legend.profile.eyeReplacement.empty());
        CHECK(legend.profile.lionStump.empty());
        auto assets = loadLegendsBossAssets(device, native, common, kind, program, {}, warnings);
        REQUIRE(assets);
        const auto targetIndex = *legend.profile.targetNode;
        REQUIRE(targetIndex < assets->tree->nodes.size());
        const auto targetName = assets->tree->nodes[targetIndex].name;
        if (index == 2) {
            REQUIRE(assets->children.size() == 3);
            CHECK(assets->children[1].rootNode() == targetName);
            const auto authored =
                formats::decodeLegendsN64Critter(readFile(native / "chi/moves.wad"));
            REQUIRE(authored.sounds.size() > 17);
            CHECK(authored.sounds[17].name == "STUMPL");
            CHECK(authored.sounds[17].flags == 0x401);
            // The source references STUMPL, but none of the native sequence
            // banks contains that group. Its independent 0x400 hide still acts.
            CHECK_FALSE(assets->archive.trees.find("STUMPL"));
            CHECK(std::ranges::any_of(warnings, [](const auto& warning) {
                return warning.find("native effect group STUMPL is absent") != std::string::npos;
            }));
            CHECK_FALSE(assets->archive.trees.find("STUMPLQ"));
        } else {
            CHECK_FALSE(assets->archive.models.find("PBOSSQEYEBALL"));
        }
        Bosses bosses;
        bosses.open(device, common, nullptr, {}, 'G', {}, &legend.profile);
        // Configuration is copied: neither caller mutation nor its destruction
        // can retarget an active encounter or its local snapshots.
        legend.profile.targetNode = 0xffffffffU;
        auto* stock = bosses.preload(kind, std::move(assets));
        REQUIRE(stock);
        REQUIRE(bosses.spawn(kind, {}, 0, 1000));
        REQUIRE(bosses.nodeTransform(targetName));
        CHECK(bosses.legendTargetTransform() == bosses.nodeTransform(targetName));
        REQUIRE(bosses.bringLegend(0));
        EnemyView player;
        player.player = 0;
        player.position = {0, 0, 40};
        player.radius = 1;
        player.height = 6;
        const std::array party{player};
        for (s32 frame = 0; frame < 1200 && !bosses.legend().thrown(); ++frame) {
            bosses.update(1, 1.f / 60, party);
        }
        REQUIRE(bosses.legend().thrown());
        CHECK(bosses.legendTargetTransform() == bosses.nodeTransform(targetName));
        const auto health = bosses.view().health;
        bosses.landLegend();
        if (index == 2) {
            REQUIRE(bosses.fighter().child(2));
            CHECK_FALSE(bosses.fighter().child(2)->alive());
            CHECK(bosses.fighter().child(1)->alive());
            CHECK(bosses.fighter().child(3)->alive());
        } else {
            CHECK(bosses.blinded());
            CHECK(bosses.view().health < health);
        }
        // No replacement model exists for the actual actor after damage to use,
        // so rewind cannot resurrect an unbound GameCube eye.
        CHECK(stock->replacementModels.empty());
        if (index == 2) {
            // Exercise the actual child death, not an injected cue: e0881b84
            // hides the branch before attempting to open the missing STUMPL.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            auto& actor = const_cast<Combatant&>(bosses.fighter());
            auto cues = bosses.takeCues();
            REQUIRE(actor.childCount() == 3);
            const auto* lion = actor.child(2);
            REQUIRE(lion != nullptr);
            Combatant::LocalSnapshot shown; // the actor one update before the lion was hidden
            for (s32 frame = 0; frame < 600 && !lion->hidden(); ++frame) {
                REQUIRE(actor.saveLocal(shown));
                bosses.update(1, 1.f / 60, party);
                const auto next = bosses.takeCues();
                cues.insert(cues.end(), next.begin(), next.end());
                REQUIRE(actor.childCount() == 3);
            }
            REQUIRE(lion->hidden());
            CHECK_FALSE(actor.child(1)->hidden());
            CHECK_FALSE(actor.child(3)->hidden());
            CHECK(std::ranges::any_of(cues, [](const auto& cue) { return cue.tree == "STUMPL"; }));
            CHECK(
                std::ranges::none_of(cues, [](const auto& cue) { return cue.tree == "STUMPLQ"; }));
            // Pin the actual render effect. The actor an update earlier, the dying
            // lion still shown, must add its submitted geometry; the real state
            // removes it and leaves the other heads/body drawing.
            const auto drawnVertices = [&] {
                stock->body.resetTextures();
                device.draws.clear();
                bosses.draw(device, Mat4{1}, {});
                usize vertices = 0;
                for (const auto& draw : device.draws) {
                    vertices += draw.vertices.size();
                }
                return vertices;
            };
            const auto hiddenVertices = drawnVertices();
            REQUIRE(hiddenVertices > 0);
            Combatant::LocalSnapshot hidden;
            REQUIRE(actor.saveLocal(hidden));
            REQUIRE(actor.restoreLocal(shown));
            CHECK(drawnVertices() > hiddenVertices);
            REQUIRE(actor.restoreLocal(hidden));
            CHECK(drawnVertices() == hiddenVertices);
        }
        Bosses::LocalSnapshot saved;
        REQUIRE(bosses.saveLocal(saved));
        bosses.update(1, 1.f / 60, party);
        REQUIRE(bosses.restoreLocal(saved));
        CHECK(bosses.legendTargetTransform() == bosses.nodeTransform(targetName));
        bosses.close();
        CHECK_FALSE(bosses.restoreLocal(saved));
        CHECK_FALSE(bosses.legendTargetTransform());
    }
}

TEST_CASE("The native javelin follows its eye for its authored attachment lifetime and rewinds",
          "[legends][native-legends][legend-profile][native-legend-attachment][assets]") {
    const auto root = nativeRoot();
    std::vector<std::string> warnings;
    auto binding = loadLegendsLegend(root, 3, readFile(root / "_program/game.bin"), warnings);
    REQUIRE(binding.archive);
    REQUIRE(binding.profile.impactAttachmentSeconds == (378.f - 255.f) / 30.f);
    test::FakeRenderDevice device;
    ItemArchive weapons;
    TextureSet textures;
    EffectTrees effects;
    std::vector<LegendSound> sounds;
    LegendPresentation presentation(effects,
                                    {device, *binding.archive, weapons, textures, &binding.profile},
                                    {.mapped = [&](LegendSound phase) {
                                        sounds.push_back(phase);
                                        return true;
                                    }});
    LegendPresentation::Bearer bearer;
    bearer.player = 0;
    bearer.facing = {0, 0, 1};
    bearer.holdPoint = {0, 3, 0};
    bearer.canGesture = true;
    LegendPresentation::Target target;
    target.position = {0, 8, 10};
    target.attachment = glm::translate(Mat4{1}, target.position);
    target.root = glm::translate(Mat4{1}, Vec3{100, -20, 0}); // must not use boss root +6Y
    target.touches = [](const Vec3&, const Vec3&, f32) { return true; };
    presentation.show(LegendCue::Brandished, 0, legendRealmOf(38), 38, bearer);
    presentation.show(LegendCue::Thrown, 0, legendRealmOf(38), 38, bearer);
    bearer.released = true;
    CHECK_FALSE(presentation.update(0, bearer, target).landed);
    REQUIRE(presentation.update(1.f / 60, bearer, target).landed);
    u32 attached = 0;
    for (usize i = 0; i < effects.count(); ++i) {
        const auto& effect = effects.effect(i);
        if (effect.name == binding.profile.projectile && effect.attachment == target.attachment) {
            attached = effect.id;
        }
    }
    REQUIRE(attached != 0);
    const auto find = [&]() -> const EffectTrees::Effect* {
        for (usize i = 0; i < effects.count(); ++i) {
            if (effects.effect(i).id == attached) {
                return &effects.effect(i);
            }
        }
        return nullptr;
    };
    REQUIRE(find());
    CHECK(find()->transform() == *target.attachment);
    CHECK(find()->secondsLeft == binding.profile.impactAttachmentSeconds);
    LegendPresentation::LocalSnapshot saved;
    EffectTrees::LocalSnapshot effectSaved;
    presentation.saveLocal(saved);
    effects.saveLocal(effectSaved);
    const auto original = test::observeEffect(effects, attached);
    const auto soundCount = sounds.size();
    const auto camera = CameraFrame::of(WorldCamera{});
    effects.draw(device, Mat4{1}, {}, &camera);
    REQUIRE_FALSE(device.draws.empty());

    target.position = {5, 12, -4};
    target.attachment = glm::rotate(glm::translate(Mat4{1}, target.position), .71f, Vec3{0, 1, 0});
    target.attachment = glm::rotate(*target.attachment, -.37f, Vec3{1, 0, 0});
    effects.update(4.f);
    CHECK_FALSE(presentation.update(0, bearer, target).landed);
    REQUIRE(find());
    CHECK(find()->transform() == *target.attachment);
    effects.update(.11f);
    CHECK_FALSE(presentation.update(0, bearer, target).landed);
    CHECK_FALSE(effects.playing(attached));

    REQUIRE(effects.restoreLocal(effectSaved));
    REQUIRE(presentation.restoreLocal(saved));
    CHECK(test::observeEffect(effects, attached) == original);
    CHECK(sounds.size() == soundCount);
    CHECK_FALSE(presentation.update(0, bearer, target).landed);
    REQUIRE(find());
    CHECK(find()->transform() == *target.attachment);
    CHECK(find()->secondsLeft == binding.profile.impactAttachmentSeconds);
    effects.update(4.f);
    REQUIRE(effects.playing(attached));
    effects.update(.11f);
    presentation.update(0, bearer, target);
    CHECK_FALSE(effects.playing(attached));
    CHECK(std::ranges::count(sounds, LegendSound::Landed) == 1);

    REQUIRE(effects.restoreLocal(effectSaved));
    REQUIRE(presentation.restoreLocal(saved));
    CHECK_FALSE(presentation.update(0, bearer, std::nullopt).landed);
    CHECK_FALSE(effects.playing(attached)); // missing/destroyed eye cannot leave orphan art
    presentation.clear();
    CHECK_FALSE(presentation.restoreLocal(saved));
}

TEST_CASE("Native legendary rites consume only their bearer and native campaign identity",
          "[legends][native-legends][assets]") {
    const auto root = nativeRoot();
    constexpr std::array kMaps{"mount6", "ice5", "castle5", "town5"};
    for (s32 index = 0; index < 4; ++index) {
        CAPTURE(index);
        const auto directory = root / "levels" / kMaps[static_cast<usize>(index)];
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                 readFile(directory / "objects.wad"),
                                                 readFile(directory / "ipr.rom"));
        test::FakeRenderDevice device;
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        LegendsLevel level(device, map, std::move(adapter), directory, test::dataDirectory());
        CharacterSave bearer;
        bearer.character = 0;
        auto& native = bearer.progress().legendsProgress(LegendsPlatform::N64);
        auto& dreamcast = bearer.progress().legendsProgress(LegendsPlatform::Dreamcast);
        dreamcast.legendItems = 0x1f;
        CHECK_FALSE(level.hasBossLegend(bearer));
        native.legendItems = 0x1f;
        const auto gc = bearer.progress().relics;
        CharacterSave other = bearer;
        REQUIRE(level.hasBossLegend(bearer));
        REQUIRE(level.bossLegendArchive());
        REQUIRE(level.bossLegendProfile());
        CHECK(level.consumeBossLegend(bearer));
        CHECK_FALSE(level.consumeBossLegend(bearer));
        CHECK(native.legendItems == (0x1fU & ~(u64{1} << index)));
        CHECK(other.progress().legendsProgress(LegendsPlatform::N64).legendItems == 0x1f);
        CHECK(dreamcast.legendItems == 0x1f);
        CHECK(bearer.progress().relics == gc);
    }
}

TEST_CASE("The common opponent and boss sequence owners consume and apply a native legendary rite",
          "[legends][native-legends][legend-bridge][assets]") {
    const auto native = nativeRoot();
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    const auto directory = native / "levels/mount6";
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    auto imported =
        std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, root);
    CharacterSave save;
    save.progress().legendsProgress(LegendsPlatform::N64).legendItems = 0x1f;
    save.progress().legendsProgress(LegendsPlatform::Dreamcast).legendItems = 0x1f;
    const auto gc = save.progress().relics;
    LegendsTraversal traversal;
    traversal.open(map, 0, save);
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:mount6";
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                               traversal.collision(), imported));
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    audio.open(root, nullptr, nullptr);
    std::array<PlayerRuntime, 2> players;
    const auto position = imported->importedBoss()->position + Vec3{0, 0, 10};
    // Controller identity, not party vector order, chooses the native bearer.
    players[0].actor.spawn(3, save, nullptr, position, 0);
    players[1].actor.spawn(1, save, nullptr, position + Vec3{5, 0, 0}, 0);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root}, players);
    REQUIRE(opponents.bosses().legend().player() == 1);
    REQUIRE(opponents.bosses().legend().stage() == LegendRite::Stage::Carried);
    BossSequence sequence;
    sequence.bind({device, world, weapons, world.textures(), effects, audio});
    usize brandished = 0;
    usize thrown = 0;
    test::OpponentScript events;
    test::ScriptedOpponents host{events};
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.settleBlasts = [] {};
    events.legend = [&](const LegendEvent& event) {
        CHECK(event.player == 1);
        if (event.cue == LegendCue::Brandished) {
            ++brandished;
            CHECK_FALSE(imported->hasBossLegend(players[1].actor.save()));
            CHECK(imported->hasBossLegend(players[0].actor.save()));
        }
        thrown += event.cue == LegendCue::Thrown ? 1 : 0;
        sequence.showLegend(event, opponents.bosses(), players);
    };
    events.advanceLegend = [&](f32 seconds) {
        sequence.advanceLegend(seconds, opponents.bosses(), players);
    };
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.destroyedGenerator = [](s32) {};
    events.shake = [] {};
    events.help = [](s32, usize) { return false; };
    const f32 health = opponents.bosses().view().health;
    bool frozen = false;
    for (s32 frame = 0; frame < 1800 && !frozen; ++frame) {
        effects.update(1.f / 60);
        opponents.update(1, 1.f / 60, players, {}, host, host, host);
        frozen = opponents.bosses().frozen();
    }
    REQUIRE(brandished == 1);
    REQUIRE(thrown == 1);
    REQUIRE(frozen);
    CHECK(opponents.bosses().view().health < health);
    CHECK(players[0].actor.save().progress().legendsProgress(LegendsPlatform::N64).legendItems ==
          0x1f);
    CHECK(players[1].actor.save().progress().legendsProgress(LegendsPlatform::N64).legendItems ==
          0x1e);
    for (const auto& player : players) {
        CHECK(player.actor.save()
                  .progress()
                  .legendsProgress(LegendsPlatform::Dreamcast)
                  .legendItems == 0x1f);
        CHECK(player.actor.save().progress().relics == gc);
    }
    sequence.clear();
    opponents.close();
    audio.close();
}
