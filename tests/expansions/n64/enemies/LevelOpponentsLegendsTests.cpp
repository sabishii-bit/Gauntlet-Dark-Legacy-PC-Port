#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/critters/LegendsCritters.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/combat/DamageTypes.h"
#include "game/effects/EnemyFeedbackEmission.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/LevelOpponents.h"
#include "game/level/ImportedLevel.h"
#include "game/party/PlayerPowerups.h"

namespace {
using namespace gdl;
using namespace gdl::game;
class NativeActors final : public ImportedLevel {
public:
    std::vector<ImportedActor> roster;
    mutable const WorldCollision* boundCollision = nullptr;
    void bindCollision(WorldCollision& collision) const override { boundCollision = &collision; }
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
    std::span<const ImportedActor> importedActors() const override { return roster; }
};
LevelOpponents::Events safeEvents() {
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.legend = [](const LegendEvent&) {};
    events.shake = [] {};
    events.help = [](s32, usize) { return false; };
    events.destroyedGenerator = [](s32) {};
    return events;
}
} // namespace

TEST_CASE("Native Town suicide barrels release damaging poison instead of the fire burst",
          "[legends][level-opponents][native-suicide-gas][assets]") {
    std::string mapName;
    s32 kind = 0;
    bool poison = false;
    SECTION("Town green barrel uses its native poison policy") {
        mapName = "town1";
        kind = 13; // Native ZOM maps to the common zombie kind.
        poison = true;
    }
    SECTION("Mountain barrel retains fire") {
        mapName = "mount1";
        kind = 1; // Native TRO maps to the common troll kind.
    }
    CAPTURE(mapName);
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels" / mapName;
    if (!std::filesystem::is_regular_file(directory / "worlds.wad"))
        SKIP("Native N64 extraction is not installed");
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    REQUIRE_FALSE(map.starts.empty());
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter);
    auto native = std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, root);
    REQUIRE(native->suicideUsesGas() == poison);
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:" + mapName;
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                               traversal.collision(), native));
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    const Vec3 ground = traversal.actor().position();
    players[0].actor.spawn(0, {}, nullptr, ground, 0);
    players[0].actor.save().progress().health = 1000;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    // Isolate this actual native bomber without allowing nearby stage broods
    // or pending placed actors to add unrelated melee damage to the observation.
    opponents.generators().clear();
    opponents.setUnseen(0, true);
    REQUIRE(opponents.enemies().loadKind(kind));
    // Native actor algorithm18 maps explicitly to kSuicideWay. EnemySpawn's
    // default -1 instead requests the ordinary kind's chase algorithm, even
    // when tier6 selects the barrel-carrying artwork.
    const auto bomber =
        opponents.enemies().spawn(EnemySpawn{.kind = kind,
                                             .tier = kSuicideStrength,
                                             .algorithm = kSuicideWay,
                                             .position = ground + Vec3{2, 0, 0},
                                             .placed = true,
                                             .priority = EnemySpawn::Priority::Visible},
                                  {});
    REQUIRE(bomber);
    REQUIRE(opponents.enemies().algorithmOf(*bomber) == kSuicideWay);
    std::vector<HurtKind> hurts;
    auto events = safeEvents();
    events.hurt = [&](usize, f32, HurtKind hurt, bool, const PlayerImpact&) {
        hurts.push_back(hurt);
    };
    opponents.strikeEnemy(*bomber, 1000, 0, Vec3{0, 0, 1}, 0, players);
    std::set<std::string> shown;
    for (s32 frame = 0; frame < 150; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
        effects.update(1.0f / 30);
        for (usize i = 0; i < effects.count(); ++i)
            shown.insert(effects.effect(i).name);
    }
    CHECK(shown.contains("SUICIDEEXP"));
    if (poison) {
        CHECK(shown.contains("POISONEXP1"));
        CHECK(shown.contains("POISONEXP2"));
        CHECK(shown.contains("POISONEXP3"));
        CHECK_FALSE(shown.contains("EXPLOSION"));
        CHECK_FALSE(shown.contains("EXPRING"));
        CHECK(hurts.size() >= 3);
        CHECK(std::ranges::all_of(hurts, [](HurtKind hurt) { return hurt == HurtKind::Gas; }));
    } else {
        CHECK(shown.contains("EXPLOSION"));
        CHECK(shown.contains("EXPRING"));
        CHECK_FALSE(shown.contains("POISONEXP1"));
        CHECK_FALSE(hurts.empty());
        CHECK(std::ranges::none_of(hurts, [](HurtKind hurt) { return hurt == HurtKind::Gas; }));
    }
    effects.clear();
    opponents.close();
}

TEST_CASE("Aggressor elemental bags release collectible potions on imported levels",
          "[legends][aggressor-drops][level-opponents][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount2";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("native N64 extraction is not installed");
    }
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                   readFile(directory / "objects.wad"),
                                                   readFile(directory / "ipr.rom"));
    REQUIRE_FALSE(map.starts.empty());
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:mount2";
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                               traversal.collision(), std::make_shared<NativeActors>()));
    REQUIRE(world.layout().itemInfos().empty());
    REQUIRE(world.placedItems().size() == 0);
    ItemArchive weapons;
    bool bagArt = false;
    SECTION("the visible bag finishes before its potion is released") {
        REQUIRE(weapons.load(root / "WEAPONS"));
        bagArt = true;
    }
    SECTION("missing optional bag art does not discard the potion") {}
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    const Vec3 position = map.starts.front().position;
    players[0].actor.spawn(0, {}, nullptr, position + Vec3{20, 0, 0}, 0);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.enemies().loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    spawn.position = position;
    spawn.elementalChance = 1;
    const auto id = opponents.enemies().spawn(spawn, {});
    REQUIRE(id);
    const auto element = opponents.enemies().elementOf(*id);
    REQUIRE(element >= 1);
    REQUIRE(element <= 4);
    opponents.strikeEnemy(*id, 1.0e6f, static_cast<u32>(element % 4 + 1), Vec3{0, 0, 1}, 0,
                          players);
    REQUIRE_FALSE(opponents.enemies().alive(*id));
    const auto events = safeEvents();
    opponents.update(1, 1.0f / 60, players, {}, events);
    if (bagArt) {
        REQUIRE(opponents.bagCount() == 1);
        CHECK(world.placedItems().size() == 0);
    }
    for (s32 tick = 0; tick < 240; ++tick) {
        effects.update(1.0f / 60);
        world.update(1.0f / 60, false);
        opponents.update(1, 1.0f / 60, players, {}, events);
    }
    CHECK(opponents.bagCount() == 0);
    REQUIRE(world.placedItems().size() == 1);
    const auto& item = world.placedItems().item(0);
    CHECK(item.name == "POT_" + std::string{damage::colourOf(static_cast<u32>(element))});
    REQUIRE(item.takeable());
    CHECK(item.flags == static_cast<u32>(element));
    CHECK(item.value == 1);
    CharacterSave recipient;
    const auto taken = takeItem(recipient, {item.subtype, item.value, item.flags, item.strength});
    REQUIRE(taken.took());
    REQUIRE(recipient.progress().inventory.potions.size() == 1);
    CHECK(recipient.progress().inventory.nextPotion() == element);
}

TEST_CASE("Native N64 authored actors activate through the common opponent owner and combat pools",
          "[legends][legends-actors][level-opponents][assets]") {
    const auto nativeRoot = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = nativeRoot / "levels/mount2";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("native N64 extraction is not installed");
    }
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    const auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                   readFile(directory / "objects.wad"),
                                                   readFile(directory / "ipr.rom"));
    const auto decoded = decodeLegendsN64Actors(
        readFile(directory / "ipr.rom"), readFile(nativeRoot / "items/generic/pgen.rom"),
        readFile(nativeRoot / "_program/game.bin"), "mount2", map.collision);
    REQUIRE(decoded.warnings.empty());
    REQUIRE(decoded.actors.size() == 20); // one golem and nineteen native ordinary actors
    bool golem = true;
    SECTION("golem counterpart remains a pending statue before native proximity") {}
    SECTION("ordinary authored enemy uses its same-position common swarm counterpart") {
        golem = false;
    }
    CAPTURE(golem);
    const auto found = std::ranges::find_if(decoded.actors, [&](const ImportedActor& actor) {
        return golem ? actor.spawn.kind == kGolemEnemyKind : actor.spawn.kind < kSwarmKindCount;
    });
    REQUIRE(found != decoded.actors.end());
    const ImportedActor descriptor = *found;
    auto native = std::make_shared<NativeActors>();
    native->roster.push_back(descriptor); // isolate one real authored placement, never move it
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:mount2";
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                               traversal.collision(), native));
    CHECK(native->boundCollision == &world.collision());
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(
        2, {}, nullptr, descriptor.spawn.position + Vec3{descriptor.activationRadius + 2, 0, 0}, 0);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root}, players);
    const auto& pending = opponents.importedActors();
    REQUIRE(pending.count() == 1);
    REQUIRE(pending.warnings().empty());
    REQUIRE(pending.figure(0).hasFigure());
    CHECK(pending.pending(0));
    CHECK(opponents.critters().count() == 0);
    CHECK(opponents.critters().targets().empty());
    CHECK(opponents.enemies().count() == 0);
    const auto events = safeEvents();
    const auto tick = [&] { opponents.update(1, 1.0f / 60, players, {}, events); };
    tick();
    CHECK(pending.pending(0));

    // Paused rendering and floor synchronization do not authorize gameplay.
    const auto frozen = pending.figure(0).checkpoint();
    REQUIRE(frozen);
    for (s32 frame = 0; frame < 5; ++frame) {
        opponents.syncFloors();
        device.draws.clear();
        pending.draw(device, Mat4{1}, WorldLighting{}, nullptr, TreeModel::Pass::All, 1);
        CHECK_FALSE(device.draws.empty());
        const auto held = pending.figure(0).checkpoint();
        REQUIRE(held);
        CHECK(held->animation == frozen->animation);
        CHECK(held->pose == frozen->pose);
        CHECK(held->textures == frozen->textures);
        CHECK(held->position == frozen->position);
        CHECK(held->transform == frozen->transform);
    }
    players[0].actor.place(descriptor.spawn.position + Vec3{0, 0, 10});
    players[0].departed = true;
    tick();
    CHECK(pending.pending(0));
    players[0].departed = false;
    players[0].life = PlayerLife::Dying;
    tick();
    CHECK(pending.pending(0));
    players[0].life = PlayerLife::InTower;
    tick();
    CHECK(pending.pending(0));
    players[0].life = PlayerLife::Standing;
    auto& inventory = players[0].actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 5);
    REQUIRE(inventory.powerupCount() == 1);
    inventory.powerups[0].on = true;
    REQUIRE(PlayerPowerups::timeStopped(players));
    tick();
    CHECK(pending.pending(0));
    inventory.powerups[0].on = false;
    tick();
    REQUIRE_FALSE(pending.pending(0));
    device.draws.clear();
    pending.draw(device, Mat4{1}, WorldLighting{});
    CHECK(device.draws.empty()); // old native placeholder is not duplicated after spawn
    if (golem) {
        REQUIRE(opponents.critters().count() == 1);
        REQUIRE_FALSE(opponents.critters().targets().empty());
        CHECK(opponents.enemies().count() == 0);
        const auto* data = opponents.critters().dataOf(0);
        REQUIRE(data != nullptr);
        usize checked = 0;
        for (const auto& cue : data->sounds()) {
            auto sound = cue.soundFormat;
            const auto marker = sound.find("%c");
            if (marker == std::string::npos || !sound.starts_with("S_GOL")) {
                continue;
            }
            sound.replace(marker, 2, 1, descriptor.costumeRealm);
            INFO(sound);
            CHECK(audio.hasNamed(sound));
            ++checked;
        }
        CHECK(checked >= 3);
        const f32 before = opponents.critters().healthOf(0);
        opponents.strikeCritter(0, 10000, 0, {0, 0, 1}, 2, std::nullopt, true, players);
        CHECK(opponents.critters().healthOf(0) < before);
    } else {
        REQUIRE(opponents.enemies().count() == 1);
        REQUIRE_FALSE(opponents.enemies().targets().empty());
        CHECK(opponents.critters().count() == 0);
        CHECK(opponents.enemies().kindOf(0) == descriptor.spawn.kind);
        CHECK(opponents.enemies().algorithmOf(0) == descriptor.spawn.algorithm);
        REQUIRE(opponents.levelNow() != nullptr);
        EnemyFeedback feedback;
        feedback.kind = descriptor.spawn.kind;
        feedback.tier = descriptor.spawn.tier;
        const auto sound = feedback.sound(opponents.levelNow()->enemies);
        REQUIRE_FALSE(sound.empty());
        CHECK(audio.hasNamed(sound));
        opponents.strikeEnemy(0, 10000, 0, {0, 0, 1}, 2, players);
        CHECK_FALSE(opponents.enemies().alive(0));
    }
    tick();
    CHECK_FALSE(pending.pending(0)); // killing the replacement never reinstalls the placement
    opponents.close();
    CHECK(opponents.importedActors().count() == 0);
}

TEST_CASE("Native It stays visible through the real imported level and common tagging path",
          "[legends][legends-actors][legends-it][level-opponents][assets]") {
    const auto nativeRoot = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = nativeRoot / "levels/mount3";
    if (!std::filesystem::is_regular_file(nativeRoot / "it/objects.wad")) {
        SKIP("native N64 extraction is not installed");
    }
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 3);
    auto native = std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, root);
    const auto found = std::ranges::find_if(
        native->importedActors(), [](const auto& actor) { return actor.spawn.kind == kItKind; });
    REQUIRE(found != native->importedActors().end());
    const usize index = static_cast<usize>(found - native->importedActors().begin());
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:mount3";
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                               traversal.collision(), native));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 3> players;
    for (usize player = 0; player < players.size(); ++player) {
        players[player].actor.spawn(static_cast<s32>(player), {}, nullptr,
                                    found->spawn.position + Vec3{0, 0, 12}, 0);
    }
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root}, players);
    const auto* archive = opponents.enemies().archive(kItKind);
    REQUIRE(archive);
    const auto& actors = opponents.importedActors();
    REQUIRE(index < actors.count());
    REQUIRE(actors.figure(index).presentation().tree);
    CHECK(actors.figure(index).presentation().tree->name == "IT1");
    CHECK(actors.pending(index));
    const auto drawsIt = [&] {
        device.draws.clear();
        // Pending placement and live enemy now share the translated native
        // archive. Activation transfers drawing to Enemies, without a replica.
        actors.draw(device, Mat4{1}, WorldLighting{});
        opponents.enemies().draw(device, Mat4{1}, WorldLighting{});
        usize vertices = 0;
        for (const auto& draw : device.draws) {
            for (u32 texture = 0; texture < archive->textures.size(); ++texture) {
                if (draw.texture == archive->textures.cachedTexture(texture)) {
                    vertices += draw.vertices.size();
                    break;
                }
            }
        }
        return vertices;
    };
    CHECK(drawsIt() == 6); // the authored two-triangle billboard is visible while pending
    const auto events = safeEvents();
    opponents.update(1, 1.0f / 60, players, {}, events);
    REQUIRE_FALSE(actors.pending(index));
    s32 live = -1;
    for (s32 slot = 0; slot < opponents.enemies().most(); ++slot) {
        if (opponents.enemies().alive(slot) && opponents.enemies().kindOf(slot) == kItKind) {
            live = slot;
            break;
        }
    }
    REQUIRE(live >= 0);
    CHECK(drawsIt() == 6); // exactly one native body, not both pending and live copies
    for (s32 tick = 0; tick < 180 && opponents.enemies().alive(live); ++tick) {
        const Vec3 position = opponents.enemies().positionOf(live);
        for (auto& player : players) {
            player.actor.place(position + Vec3{0, 0, .5f});
        }
        opponents.update(1, 1.0f / 60, players, {}, events);
    }
    CHECK_FALSE(opponents.enemies().alive(live));
    CHECK_FALSE(opponents.enemies().presentation(live));
    if (const auto retiring = opponents.enemies().observe(live)) {
        CHECK(retiring->alpha == 0);
    }
    CHECK(std::ranges::count_if(players, [](const auto& player) { return player.itTicks != 0; }) ==
          1);
    CHECK(drawsIt() == 0);
    CHECK_FALSE(actors.pending(index));
    opponents.close();
}
