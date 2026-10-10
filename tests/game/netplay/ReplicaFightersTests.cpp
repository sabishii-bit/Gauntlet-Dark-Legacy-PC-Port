#include <algorithm>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Gargoyle.h"
#include "game/enemies/General.h"
#include "game/enemies/Golem.h"
#include "game/enemies/HeadedBodyFixture.h"
#include "game/netplay/CombatPlayback.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/ReplicaFighters.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

CombatSnapshot blank(u64 tick = 0) {
    CombatSnapshot value;
    value.motion.epoch = value.motion.cameraContinuity = 1;
    value.motion.tick = tick;
    return value;
}
FighterMeshState mesh() {
    FighterMeshState state;
    state.incarnation = state.part = state.resource = 1;
    state.nodes.push_back({});
    return state;
}
void word(std::vector<u8>& bytes, usize offset, u32 value) {
    for (u32 i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}
void sameDraws(std::span<const test::RecordedDraw> actual,
               std::span<const test::RecordedDraw> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize i = 0; i < actual.size(); ++i) {
        CAPTURE(i);
        const auto& a = actual[i];
        const auto& b = expected[i];
        REQUIRE(a.texture == b.texture);
        CHECK(a.state.maskedTexture == b.state.maskedTexture);
        CHECK(a.state.effectiveTextureBlend() == Approx(b.state.effectiveTextureBlend()));
        CHECK(a.state.alphaTest == b.state.alphaTest);
        CHECK(a.state.alphaToCoverage == b.state.alphaToCoverage);
        CHECK(a.state.uvOffset == b.state.uvOffset);
        CHECK(a.state.uvScale == b.state.uvScale);
        CHECK(a.state.cullBack == b.state.cullBack);
        CHECK(a.state.colorScale == b.state.colorScale);
        CHECK(a.state.depthTest == b.state.depthTest);
        CHECK(a.state.depthWrite == b.state.depthWrite);
        CHECK(a.blend() == b.blend());
        REQUIRE(a.vertices.size() == b.vertices.size());
        for (usize v = 0; v < a.vertices.size(); ++v) {
            const auto av = a.transform * Vec4{a.vertices[v].position, 1};
            const auto bv = b.transform * Vec4{b.vertices[v].position, 1};
            for (s32 axis = 0; axis < 3; ++axis) {
                CHECK(av[axis] == Approx(bv[axis]).margin(0.001f));
            }
            CHECK(a.vertices[v].color == b.vertices[v].color);
            CHECK(a.vertices[v].uv.x == Approx(b.vertices[v].uv.x).margin(0.0001f));
            CHECK(a.vertices[v].uv.y == Approx(b.vertices[v].uv.y).margin(0.0001f));
        }
    }
}
void compare(test::FakeRenderDevice& device, const Combatant& actor, FighterResources& resources,
             const CameraFrame* camera = nullptr) {
    test::FakeTexture flash{1, 1};
    test::FakeTexture frozen{1, 1};
    WorldLighting lighting;
    lighting.ambient = Vec3{0.25f};
    lighting.lightColor = Vec3{0.4f};
    device.draws.clear();
    actor.draw(device, Mat4{1}, lighting, &frozen, camera, &flash);
    const auto expected = device.draws;
    const auto textures = device.texturesCreated;
    auto state = blank();
    for (const auto& visual : actor.visuals(camera)) {
        const auto shown = resources.capture(visual, 0, actor.incarnation());
        CAPTURE(visual.part, visual.sequence, visual.frame);
        REQUIRE(shown);
        state.fighters.push_back(*shown);
    }
    std::ranges::sort(state.fighters, {}, &FighterMeshState::key);
    const auto encoded = CombatPacket::encode(state);
    REQUIRE(encoded);
    const auto decoded = CombatPacket::decode(*encoded);
    REQUIRE(decoded);
    ReplicaFighters replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(*decoded, resources));
    for (s32 repeat = 0; repeat < 2; ++repeat) {
        device.draws.clear();
        replica.draw(device, resources, Mat4{1}, lighting, &flash, &frozen);
        sameDraws(device.draws, expected);
    }
    CHECK(device.texturesCreated == textures);
}
TEST_CASE("fighter codec bounds variable poses before allocation and rejects malformed state",
          "[netplay][fighter-snapshot]") {
    std::vector<FighterMeshState> state{mesh()};
    state[0].incarnation = 0x123456789ABCDEFULL;
    state[0].nodes[0].generation = 0x123456789ABCDEFULL;
    state[0].nodes[0].transform = glm::translate(Mat4{1}, Vec3{1, 2, 3});
    const auto bytes = FighterPacket::encode(state);
    REQUIRE(bytes);
    CHECK(bytes->size() ==
          FighterPacket::kHeaderBytes + FighterPacket::kMeshBytes + FighterPacket::kNodeBytes);
    const auto decoded = FighterPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(FighterPacket::encode(*decoded) == bytes);
    for (usize length = 0; length < bytes->size(); ++length) {
        CHECK_FALSE(FighterPacket::decode(std::span(*bytes).first(length)));
    }
    auto invalid = *bytes;
    invalid.push_back(0);
    CHECK_FALSE(FighterPacket::decode(invalid));
    for (const usize offset : {usize{0}, usize{4}, FighterPacket::kHeaderBytes + 96}) {
        invalid = *bytes;
        word(invalid, offset, 0xFFFFFFFF);
        CHECK_FALSE(FighterPacket::decode(invalid));
    }
    invalid = *bytes;
    word(invalid, FighterPacket::kHeaderBytes + FighterPacket::kMeshBytes + 68, 2);
    CHECK_FALSE(FighterPacket::decode(invalid));
    state[0].nodes[0].frame = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(FighterPacket::encode(state));
    state = {mesh(), mesh()};
    CHECK_FALSE(FighterPacket::encode(state));
    state[1].part = 2;
    state[1].incarnation = 2;
    CHECK_FALSE(FighterPacket::encode(state));
    state[1].incarnation = 1;
    REQUIRE(FighterPacket::encode(state));
    state[0].nodes.resize(FighterPacket::kMaxTreeNodes + 1);
    CHECK_FALSE(FighterPacket::encode(state));
}
TEST_CASE("fighter playback interpolates a head only within its own uninterrupted animation",
          "[netplay][fighter-snapshot]") {
    auto first = blank(10);
    first.fighters.push_back(mesh());
    first.fighters[0].nodes[0].generation = 1;
    auto last = first;
    last.motion.tick = 14;
    last.fighters[0].placement[3].x = 8;
    last.fighters[0].nodes[0].transform[3].y = 8;
    last.fighters[0].nodes[0].frame = 4;
    last.fighters[0].nodes[0].flash = true;
    for (s32 mode = 0; mode < 5; ++mode) {
        CombatPlayback playback;
        REQUIRE(playback.begin(1, 1));
        auto end = last;
        if (mode == 1) {
            ++end.fighters[0].nodes[0].generation;
        }
        if (mode == 2) {
            ++end.fighters[0].incarnation;
        }
        if (mode == 3) {
            end.fighters.clear();
        }
        if (mode == 4) {
            ++end.motion.cameraContinuity;
        }
        for (const auto* frame : {&first, &end}) {
            const auto packets = CombatReplica::packets(*frame, SnapshotBlock::Compression::None,
                                                        CombatReplica::Recovery::None);
            REQUIRE(packets);
            for (const auto& packet : *packets) {
                playback.receive(1, packet);
            }
        }
        const auto shown = playback.sample(12);
        REQUIRE(shown);
        REQUIRE(shown->fighters.size() == 1);
        CHECK(shown->fighters[0].placement[3].x == (mode < 2 ? 4 : 0));
        CHECK(shown->fighters[0].nodes[0].transform[3].y == (mode == 0 ? 4 : 0));
        CHECK_FALSE(shown->fighters[0].nodes[0].flash);
        CHECK(CombatPacket::encode(*playback.sample(14)) == CombatPacket::encode(end));
    }
}
TEST_CASE("replicated independent heads preserve gaze damage flashes death and reincarnation",
          "[netplay][replica-fighters]") {
    test::FakeRenderDevice device;
    const auto root = test::headedBodyAssets();
    CombatantAssets stock;
    REQUIRE(stock.load(device, root, bossDefinition("CHIMERA"), 'G'));
    const std::array<const CombatantAssets*, 1> stocks{&stock};
    FighterResources resources;
    REQUIRE(resources.bind(stocks));
    Combatant actor;
    REQUIRE(actor.spawn(stock, 0, {}, 0, nullptr, {}, 'G'));
    const u64 incarnation = actor.incarnation();
    EnemyView player;
    player.player = 0;
    player.position = {10, 0, 12};
    player.height = 6;
    const std::array players{player};
    for (s32 tick = 0; tick < 240; ++tick) {
        actor.update(1, 1.0f / 60, players);
        if (tick % 20 == 0) {
            compare(device, actor, resources);
        }
    }
    EnemyHit hit;
    hit.damage = 10;
    hit.flags = 0x100000;
    hit.level = 99;
    actor.hurt(hit, 1);
    compare(device, actor, resources);
    actor.freeze(300);
    compare(device, actor, resources);
    actor.freeze(0);
    hit.damage = 10000;
    actor.hurt(hit, 1);
    actor.hurt(hit, 2);
    for (s32 tick = 0; tick < 300; ++tick) {
        actor.update(1, 1.0f / 60, players);
        if (tick % 15 == 0) {
            compare(device, actor, resources);
        }
    }
    actor.clear();
    REQUIRE(actor.spawn(stock, 0, {}, 0, nullptr, {}, 'G'));
    CHECK(actor.incarnation() > incarnation);
    compare(device, actor, resources);
    auto state = blank();
    const auto visual = actor.visuals().front();
    state.fighters.push_back(*resources.capture(visual, 0, actor.incarnation()));
    ReplicaFighters replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(state, resources));
    const auto shown = state;
    state.fighters[0].resource = 65535;
    CHECK_FALSE(replica.show(state, resources));
    CHECK(replica.count() == shown.fighters.size());
    state = shown;
    state.fighters[0].nodes.pop_back();
    CHECK_FALSE(replica.show(state, resources));
    state = shown;
    state.fighters[0].nodes[0].sequence = 65535;
    CHECK_FALSE(replica.show(state, resources));
}
TEST_CASE("native boss and great-creature replication matches the local model renderer",
          "[netplay][replica-fighters][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    std::vector<CombatantDefinition> definitions;
    for (s32 kind = 34; kind <= 44; ++kind) {
        definitions.push_back(bossDefinition(bossNameOf(kind)));
    }
    definitions.push_back(Golem::definition());
    definitions.push_back(General::definition());
    for (const auto* form : {"GAR_EAGL", "GAR_LION", "GAR_SERP"}) {
        definitions.push_back(Gargoyle::definition(form));
    }
    for (const auto& definition : definitions) {
        CAPTURE(definition.name);
        test::FakeRenderDevice device;
        CombatantAssets stock;
        REQUIRE(stock.load(device, root, definition, 'G'));
        if (definition.name == "PBOSS") {
            REQUIRE(stock.prepareReplacement(device, "EYEBALL", "PBOSSQEYEBALL"));
        }
        FighterResources resources;
        REQUIRE(resources.bind(std::array<const CombatantAssets*, 1>{&stock}));
        Combatant actor;
        REQUIRE(actor.spawn(stock, 0, {2, 3, 4}, 0.3f, nullptr, {}, 'G'));
        const auto camera = CameraFrame::of(WorldCamera{{20, 30, -20}, 0.5f, -0.5f, 0});
        const auto* facing = definition.kind == CombatantKind::Boss ? nullptr : &camera;
        compare(device, actor, resources, facing);
        EnemyView player;
        player.player = 0;
        player.position = {12, 3, 10};
        player.height = 8;
        for (s32 tick = 0; tick < 480; ++tick) {
            stock.textures.advance(1.0f / 60);
            actor.update(1, 1.0f / 60, std::array{player});
            if (tick % 137 == 0) {
                compare(device, actor, resources, facing);
            }
        }
        if (definition.name == "PBOSS") {
            REQUIRE(actor.replaceNodeModel(device, "EYEBALL", "PBOSSQEYEBALL"));
        }
        if (definition.name == "GARM") {
            for (const s32 node : {0, 1}) {
                EnemyHit broken;
                broken.damage = 1000;
                broken.node = node;
                actor.hurt(broken);
            }
            REQUIRE(actor.visuals().size() >= 3);
            compare(device, actor, resources, facing);
        }
        EnemyHit hit;
        hit.damage = 5;
        hit.flags = 0x100000;
        hit.level = 99;
        actor.hurt(hit);
        compare(device, actor, resources, facing);
        actor.freeze(300);
        compare(device, actor, resources, facing);
        actor.freeze(0);
        hit.damage = 100000;
        actor.hurt(hit);
        for (s32 tick = 0; tick < 240; ++tick) {
            actor.update(1, 1.0f / 60, {});
            if (tick % 60 == 0) {
                compare(device, actor, resources, facing);
            }
        }
    }
}
TEST_CASE("fighter capture includes the full population and rejects incomplete resource rosters "
          "atomically",
          "[netplay][replica-fighters][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    Critters critters;
    Bosses bosses;
    critters.open(device, root, nullptr, {}, 'G');
    bosses.open(device, root, nullptr, {}, 'G');
    REQUIRE(bosses.spawn(41, {100, 0, 100}, 0));
    for (s32 slot = 0; slot < Critters::kMost; ++slot) {
        const Vec3 at{static_cast<f32>(slot * 10), 0, 0};
        std::optional<s32> spawned;
        switch (slot % 3) {
        case 0: spawned = critters.spawnGolem(at, 0); break;
        case 1: spawned = critters.spawnGeneral(at, 0); break;
        default: spawned = critters.spawnGargoyle(at, 0); break;
        }
        REQUIRE(spawned == slot);
    }
    FighterResources resources;
    REQUIRE(resources.bind(critters, bosses));
    auto state = blank();
    REQUIRE(FighterCapture::append(state, resources, critters, bosses));
    for (u32 slot = 0; slot <= static_cast<u32>(Critters::kMost); ++slot) {
        CHECK(std::ranges::any_of(state.fighters,
                                  [&](const auto& shown) { return shown.actor == slot; }));
    }
    const auto saved = CombatPacket::encode(state);
    REQUIRE(saved);
    const FighterResources incomplete;
    CHECK_FALSE(FighterCapture::append(state, incomplete, critters, bosses));
    CHECK(CombatPacket::encode(state) == saved);
    ReplicaFighters replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(state, resources));
    const auto health = critters.healthOf(1);
    const auto position = critters.positionOf(1);
    const auto bossHealth = bosses.view().health;
    for (s32 i = 0; i < 4; ++i) {
        device.draws.clear();
        replica.draw(device, resources, Mat4{1}, {}, nullptr, nullptr);
        REQUIRE_FALSE(device.draws.empty());
    }
    CHECK(critters.healthOf(1) == health);
    CHECK(critters.positionOf(1) == position);
    CHECK(bosses.view().health == bossHealth);
}
TEST_CASE("offscreen Generals are registered before they enter the replicated population",
          "[netplay][replica-fighters][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG4/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G4");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1, true}, players);
    REQUIRE(opponents.critters().count() == 0);
    const auto stocks = opponents.critters().resources();
    REQUIRE(std::ranges::any_of(stocks, [](const auto* stock) {
        return stock->definition.kind == CombatantKind::General;
    }));
    FighterResources resources;
    REQUIRE(resources.bind(opponents.critters(), opponents.bosses()));
    auto state = blank();
    REQUIRE(FighterCapture::append(state, resources, opponents.critters(), opponents.bosses()));
    REQUIRE(state.fighters.empty());
    bool seen = false;
    for (const auto& instance : world.layout().itemInstances()) {
        if (instance.info < 0 ||
            static_cast<usize>(instance.info) >= world.layout().itemInfos().size()) {
            continue;
        }
        const auto& info = world.layout().itemInfos()[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kPlacedEnemy || enemyKindOf(info.name) != kGeneralEnemyKind ||
            !shownToParty(instance.minPlayers, 1)) {
            continue;
        }
        ViewVolume overhead;
        overhead.position = instance.position + Vec3{0, 10, 0};
        overhead.forward = {0, -1, 0};
        overhead.up = {0, 0, 1};
        opponents.watch(overhead, instance.position);
        seen = true;
        break;
    }
    REQUIRE(seen);
    REQUIRE(opponents.critters().count() > 0);
    CHECK(opponents.critters().resources() == stocks);
    REQUIRE(FighterCapture::append(state, resources, opponents.critters(), opponents.bosses()));
    CHECK_FALSE(state.fighters.empty());
}
} // namespace
