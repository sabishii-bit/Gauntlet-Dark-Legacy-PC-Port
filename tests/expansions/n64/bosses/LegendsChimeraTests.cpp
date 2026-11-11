#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"
#include "engine/math/PortableMath.h"
#include "engine/world/TreePose.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "expansions/n64/formats/LegendsN64Critter.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/critters/Combatant.h"
#include "game/effects/CritterCueEmission.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "chimera/objects.wad")) {
        SKIP("N64 Chimera assets are not installed");
    }
    return root;
}

struct ChimeraScene {
    test::FakeRenderDevice device;
    GameConfig config;
    StringTable strings;
    LevelWorld world;
    GameServices context; ///< what the scene reads while it stands
    PlayScene scene;
    std::shared_ptr<LegendsLevel> imported;

    /** The arena with one player carrying the campaign's `legendItems` and `potions` red
     * potions. */
    explicit ChimeraScene(u64 legendItems = 0, s32 potions = 0) {
        const auto root = test::assetOrSkip("PLAYERS").parent_path();
        const auto directory = nativeRoot() / "levels/castle5";
        const auto models = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                                 readFile(directory / "ipr.rom"));
        for (auto& texture : formats::decodeLegendsN64Textures(models)) {
            map.textures.push_back({std::move(texture.image),
                                    texture.clampU,
                                    texture.clampV,
                                    texture.hasTransparency,
                                    {}});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        REQUIRE(adapter);
        PartyMember member;
        member.player = 0;
        member.save.name = "TEST";
        member.save.progress().health = 2000;
        member.save.progress().legendsProgress(LegendsPlatform::N64).legendItems = legendItems;
        member.save.progress().inventory.addPotions(1, potions);
        const std::array party{member};
        LegendsTraversal traversal;
        traversal.open(map, 0, member.save);
        imported = std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, root);
        imported->initializeParty(party);
        auto layout = WorldLayout::fromDecoded(
            {}, {{.kind = LocatorKind::Start, .position = traversal.start()}}, map.minBounds,
            map.maxBounds);
        LevelRef ref;
        ref.realmId = -1;
        ref.name = "castle5";
        const bool loaded = world.loadImported(device, root, ref, std::move(layout),
                                               traversal.collision(), imported);
        REQUIRE(loaded);
        REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
        context.config = &config;
        context.strings = &strings;
        context.unpackedRoot = root;
        PlayOptions options;
        options.position = traversal.start();
        options.yaw = traversal.actor().yaw();
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
        scene.setCheats(0, {.god = true});
    }

    void step() { REQUIRE(scene.update(1.0 / 60, {}) == PlayOutcome::Running); }

    /** Sets the body (`id` 0) or head `id` playing its move `name` from just before its harm
     * (a head from just before its sound, the body holding its SYNC move meanwhile), aimed at
     * the first player and turned to `yaw` when one is given. */
    void forceAttack(s32 id, std::string_view name, std::optional<f32> yaw = {}) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& fighter = const_cast<Combatant&>(scene.bosses().fighter());
        const auto& stock = *scene.bosses().resources().front();
        REQUIRE(fighter.childCount() == 3);
        const auto point = [&](f32 frame) {
            return Combatant::MovePoint{.frame = frame, .target = 0, .soundsGiven = 2, .yaw = yaw};
        };
        if (id == 0) {
            const auto move = stock.data.moveNamed(name);
            REQUIRE(move);
            REQUIRE(fighter.setMove(
                *move, point(static_cast<f32>(stock.data.moves()[*move].frameStart - 1))));
        } else {
            const auto sync = stock.data.moveOfType(1);
            REQUIRE(sync);
            REQUIRE(fighter.setMove(*sync, point(0.0f)));
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            auto* child = const_cast<Combatant*>(fighter.child(id));
            REQUIRE(child);
            const auto move = child->data()->moveNamed(name);
            REQUIRE(move);
            REQUIRE(child->setMove(
                *move, point(static_cast<f32>(child->data()->moves()[*move].soundFrame - 1))));
        }
    }
};
} // namespace

TEST_CASE("Native Chimera loops its dormant idle without attacking and wakes at horizontal range",
          "[legends][native-chimera][native-chimera-entry][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    const auto common = test::assetOrSkip("CRITTER").parent_path();
    std::vector<std::string> warnings;
    auto stock = loadLegendsBossAssets(device, root, common, 35,
                                       readFile(root / "_program/game.bin"), {}, warnings);
    REQUIRE(stock);
    REQUIRE(stock->initialMoveBeforeWake);
    REQUIRE(stock->data.wakeThreshold() == 60);
    const auto initial = stock->data.moveNamed("INIT");
    REQUIRE(initial);
    REQUIRE(stock->data.moves()[*initial].anim == "REDY");
    const auto sequence = stock->tree->findSequence("REDY");
    REQUIRE(sequence);
    REQUIRE(stock->tree->sequences[*sequence].frames == 60);
    REQUIRE(stock->tree->sequences[*sequence].repeats);
    Bosses bosses;
    bosses.open(device, common, nullptr, {}, 'C');
    const auto* const preloaded = bosses.preload(35, std::move(stock));
    REQUIRE(preloaded);
    REQUIRE(bosses.spawn(35, Vec3{0}, 0));
    const auto before = bosses.nodeTransform("BODY1_EHEAD");
    REQUIRE(before);
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 80};
    player.height = 6;
    bool changed = false;
    for (int tick = 0; tick < 300; ++tick) {
        bosses.update(1, 1.0f / 60, std::span{&player, 1});
        CHECK_FALSE(bosses.view().awake);
        CHECK(bosses.moveName() == "INIT");
        CHECK(bosses.takeShots().empty());
        CHECK(bosses.takeBlows().empty());
        CHECK(bosses.takeCues().empty());
        changed |= bosses.nodeTransform("BODY1_EHEAD") != before;
    }
    CHECK(changed);
    // e0886190 measures XZ only. This point is inside60 horizontally and
    // outside60 in 3D; it must not require walking under the elevated head.
    player.position = {0, 35, 50};
    bosses.update(1, 1.0f / 60, std::span{&player, 1});
    CHECK(bosses.view().awake);
    CHECK(bosses.moveName() == "START");
}

TEST_CASE("Native Chimera backs away ten a second through its death throes",
          "[legends][native-chimera][boss-death][boss-movement][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    const auto common = test::assetOrSkip("CRITTER").parent_path();
    std::vector<std::string> warnings;
    auto stock = loadLegendsBossAssets(device, root, common, 35,
                                       readFile(root / "_program/game.bin"), {}, warnings);
    REQUIRE(stock);
    // chi/moves.wad's DEATH paces minus ten, held two seconds, which Boss.bin's translate
    // (e0883d14) takes in every state and its AI runs through the hold (e08847b0-e08847e8).
    const auto death = stock->data.moveOfType(MoveDefinition::kDeath);
    REQUIRE(death.has_value());
    CHECK(stock->data.moves()[*death].speed == -10);
    CHECK(stock->data.moves()[*death].hold == 2);
    Bosses bosses;
    bosses.open(device, common, nullptr, {}, 'C');
    REQUIRE(bosses.preload(35, std::move(stock)));
    REQUIRE(bosses.spawn(35, Vec3{0, 20, -52}, 0));
    bosses.wake();
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 500};
    EnemyHit lethal;
    lethal.damage = 100000;
    lethal.player = 0;
    bosses.hurt(lethal);
    REQUIRE(bosses.fighter().dying());
    const Vec3 fell = *bosses.position();
    Vec3 last = fell;
    s32 steps = 0;
    std::optional<Vec3> defeat;
    for (s32 tick = 0; tick < 1800 && !defeat; ++tick) {
        bosses.update(1, 1.0f / 60, std::span{&player, 1});
        bosses.takeCues();
        bosses.takeSpews();
        bosses.takeLosses();
        defeat = bosses.takeDefeat();
        if (bosses.present()) {
            CHECK(glm::length(Vec2{bosses.position()->x - last.x, bosses.position()->z - last.z}) ==
                  Approx(10.0f / 60).margin(1e-4));
            last = *bosses.position();
            ++steps;
        }
    }
    REQUIRE(defeat.has_value());
    CHECK(steps > 120);
    CHECK(glm::length(Vec2{defeat->x - fell.x, defeat->z - fell.z}) ==
          Approx(10.0f * static_cast<f32>(steps) / 60).epsilon(0.01));
}

TEST_CASE("Native Chimera attacks a grounded distant player through actual PlayScene entry",
          "[legends][native-chimera][native-chimera-gameplay][assets]") {
    ChimeraScene test;
    auto& bosses = test.scene.bosses();
    REQUIRE(bosses.present());
    REQUIRE(bosses.view().kind == 35);
    const auto* const position = bosses.position();
    REQUIRE(position);
    std::optional<Vec3> standing;
    // Use authored floor support, not a victim placed at an attack's already
    // sampled contact. Forward50 is in the native ranged-attack envelope.
    const Vec3 forward{portable::sin(bosses.facing()), 0, portable::cos(bosses.facing())};
    for (const f32 distance : {50.0f, 45.0f, 40.0f}) {
        Vec3 point = *position + forward * distance;
        point.y = test.world.layout().maxBounds().y + 10;
        if (const auto floor = test.world.collision().floorAt(point, 0, 200)) {
            point.y = floor->y;
            standing = point;
            break;
        }
    }
    REQUIRE(standing);
    bool moved = false;
    bool attacked = false;
    std::set<std::string> moves;
    const auto before = bosses.nodeTransform("BODY1_EHEAD");
    for (int tick = 0; tick < 2400 && !attacked; ++tick) {
        if (!test.scene.spawning() && !test.scene.animator(0)->entering()) {
            REQUIRE(test.scene.relocate(*standing));
        }
        test.step();
        moves.emplace(bosses.moveName());
        moved |= bosses.nodeTransform("BODY1_EHEAD") != before;
        const auto observe = [&](const Combatant& actor) {
            const auto type = actor.moveType();
            return type >= 0x80 && type <= 0x8f;
        };
        attacked |= observe(bosses.fighter());
        for (s32 id = 1; id <= 3; ++id) {
            if (const auto* head = bosses.fighter().child(id)) {
                attacked |= observe(*head);
            }
        }
    }
    CAPTURE(moves);
    CHECK(bosses.view().awake);
    CHECK(moved);
    CHECK(attacked);
    CHECK(glm::length(Vec2{test.scene.actor(0)->position().x - bosses.position()->x,
                           test.scene.actor(0)->position().z - bosses.position()->z}) > 25);
}

TEST_CASE("Native Chimera breath cues follow all three animated mouth bones without a body lift",
          "[legends][native-chimera][native-chimera-mouths][assets]") {
    const auto root = nativeRoot();
    const auto common = test::assetOrSkip("CRITTER").parent_path();
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    auto stock = loadLegendsBossAssets(device, root, common, 35,
                                       readFile(root / "_program/game.bin"), {}, warnings);
    REQUIRE(stock);
    REQUIRE(stock->children.size() == 3);
    struct Head {
        s32 id;
        const char* move;
        const char* node;
        const char* tree;
        s32 cueFrame;
        Vec3 throat;
        Vec3 nativeRest;
        Vec3 headMinimum;
        Vec3 headMaximum;
    };
    // Independent native Vtx /64 oracles: the first narrow end of each fire
    // mesh, BODY's authored root offset, and its adjacent head mesh's bounds.
    // These distinguish a correct attachment matrix from geometry already
    // translated above the mouth. Eagle's later GC asset adds X2 at SFXX;
    // native instead carries its own X-110/64 BODY translation, not a Y lift.
    const std::array heads{Head{1,
                                "E_BREATH",
                                "BODY1_EHEAD",
                                "EAGLEFIREFX",
                                8,
                                {-14.0f / 64, -66.0f / 64, 220.0f / 64},
                                {-110.0f / 64, 0, 0},
                                {-95.0f / 64, -126.0f / 64, -49.0f / 64},
                                {100.0f / 64, 113.0f / 64, 321.0f / 64}},
                           Head{2,
                                "L_BREATH",
                                "BODY1_LHEAD",
                                "LIONFIREFX",
                                5,
                                {46.0f / 64, -139.0f / 64, 122.0f / 64},
                                {},
                                {-101.0f / 64, -164.0f / 64, -42.0f / 64},
                                {106.0f / 64, 135.0f / 64, 274.0f / 64}},
                           Head{3,
                                "S_BREATH",
                                "BODY1_SHEAD",
                                "SNAKEFIREFX",
                                6,
                                {48.0f / 64, -69.0f / 64, 143.0f / 64},
                                {120.0f / 64, 0, 0},
                                {-92.0f / 64, -75.0f / 64, -45.0f / 64},
                                {94.0f / 64, 105.0f / 64, 300.0f / 64}}};
    for (const f32 yaw : {0.0f, 1.1f}) {
        for (const auto& head : heads) {
            CAPTURE(yaw, head.move);
            Combatant actor;
            const Vec3 origin{19, 7, -31};
            REQUIRE(actor.spawn(*stock, 0, origin, yaw, nullptr, {}, 'C'));
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            auto* branch = const_cast<Combatant*>(actor.child(head.id));
            REQUIRE(branch);
            const auto moveIndex = branch->data()->moveNamed(head.move);
            REQUIRE(moveIndex);
            const auto& move = branch->data()->moves()[*moveIndex];
            REQUIRE(move.colnode == head.node);
            REQUIRE(move.soundFrame == head.cueFrame);
            REQUIRE(branch->data()->sound(move.sound));
            REQUIRE(branch->data()->sound(move.sound)->flags == 0);
            REQUIRE(branch->data()->sound(move.sound)->offset == Vec3{0});
            const auto sequence = stock->tree->findSequence(move.anim);
            REQUIRE(sequence);
            REQUIRE(actor.childCount() == 3);
            REQUIRE(branch->setMove(*moveIndex, {.frame = static_cast<f32>(head.cueFrame - 1),
                                                 .soundsGiven = 2,
                                                 .yaw = std::nullopt}));
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            branch = const_cast<Combatant*>(actor.child(head.id));
            REQUIRE(branch);
            branch->update(1, 1.0f / 30, {});
            const auto cues = branch->takeCues();
            REQUIRE(cues.size() == 1);
            const auto& cue = cues.front();
            REQUIRE(cue.tree == head.tree);
            REQUIRE(cue.node == head.node);
            REQUIRE(cue.placement);
            REQUIRE(cue.follows);
            CHECK_FALSE(cue.rootAttachment);
            CHECK(cue.nodeOffset == Vec3{0});
            const auto mouth = actor.nodeTransform(head.node);
            REQUIRE(mouth);
            for (Mat4::length_type column = 0; column < 4; ++column) {
                for (Vec4::length_type row = 0; row < 4; ++row) {
                    CHECK((*cue.placement)[column][row] == Approx((*mouth)[column][row]));
                }
            }
            const auto emission = CritterCueEmission::resolve(cue, &stock->archive, stock->archive);
            EffectTrees effects;
            const auto id = effects.startSet(device, *emission.archive, emission.tree,
                                             emission.position, emission.setting);
            REQUIRE(id);
            effects.placeAt(id, *cue.placement);
            for (int frame = 0; frame < 12; ++frame) {
                if (frame > 0) {
                    branch->update(1, 1.0f / 30, {});
                    REQUIRE(branch->takeCues().empty());
                    effects.update(1.0f / 30);
                    effects.placeAt(id, *actor.nodeTransform(head.node));
                }
                REQUIRE(effects.count() == 1);
                const auto& effect = effects.effect(0);
                const auto currentMouth = actor.nodeTransform(head.node);
                REQUIRE(currentMouth);
                const auto placed = effect.transform();
                for (Mat4::length_type column = 0; column < 4; ++column) {
                    for (Vec4::length_type row = 0; row < 4; ++row) {
                        CHECK(placed[column][row] == Approx((*currentMouth)[column][row]));
                    }
                }
                const usize drawable = effect.pose.size() == 1 ? 0 : 1;
                REQUIRE(drawable < effect.pose.size());
                const Vec3 emitted{placed * effect.pose.matrices()[drawable] *
                                   Vec4{head.throat, 1}};
                const Vec3 source{*currentMouth * Vec4{head.throat + head.nativeRest, 1}};
                CHECK(glm::distance(emitted, source) < .001f);
                const Vec3 local{glm::inverse(*currentMouth) * Vec4{emitted, 1}};
                CHECK(local.y >= head.headMinimum.y - .01f);
                CHECK(local.y <= head.headMaximum.y + .01f);
                CHECK(local.z >= head.headMinimum.z);
                CHECK(local.z <= head.headMaximum.z);
                device.draws.clear();
                const auto camera = CameraFrame::at(origin + Vec3{0, 30, -40});
                effects.draw(device, Mat4{1}, {}, &camera);
                CHECK_FALSE(device.draws.empty());
                bool submittedNativeMouth = false;
                for (const auto& draw : device.draws) {
                    for (const auto& vertex : draw.vertices) {
                        submittedNativeMouth |=
                            glm::distance(Vec3{draw.transform * Vec4{vertex.position, 1}}, source) <
                            .001f;
                    }
                }
                CHECK(submittedNativeMouth);
            }
        }
    }
}

TEST_CASE("Native Chimera scene breath remains at the rendered mouth between simulation ticks",
          "[legends][native-chimera][native-chimera-interpolation][assets]") {
    ChimeraScene test;
    for (int tick = 0; tick < 1200; ++tick) {
        test.step();
        if (!test.scene.spawning() && test.imported->opponentsReady() &&
            !test.scene.animator(0)->entering()) {
            break;
        }
    }
    REQUIRE_FALSE(test.scene.spawning());
    REQUIRE(test.imported->opponentsReady());
    REQUIRE_FALSE(test.scene.animator(0)->entering());
    test.scene.bosses().wake();
    REQUIRE(test.scene.bosses().resources().size() == 1);
    auto& stock = *test.scene.bosses().resources().front();
    struct Head {
        s32 id;
        const char* move;
        const char* node;
        const char* effect;
        Vec3 throat;
        usize meshNode;
    };
    const std::array heads{
        Head{
            1, "E_BREATH", "BODY1_EHEAD", "EAGLEFIREFX", {-124.f / 64, -66.f / 64, 220.f / 64}, 91},
        Head{2, "L_BREATH", "BODY1_LHEAD", "LIONFIREFX", {46.f / 64, -139.f / 64, 122.f / 64}, 117},
        Head{3,
             "S_BREATH",
             "BODY1_SHEAD",
             "SNAKEFIREFX",
             {168.f / 64, -69.f / 64, 143.f / 64},
             104}};
    for (const auto& head : heads) {
        CAPTURE(head.move);
        test.forceAttack(head.id, head.move);
        bool rendered = false;
        f32 greatestSeparation = 0;
        f32 persistentSeparation = 0;
        for (int tick = 0; tick < 16; ++tick) {
            test.step();
            const auto& fighter = test.scene.bosses().fighter();
            const auto node = stock.tree->findNode(head.node);
            REQUIRE(node);
            const EffectTrees::Effect* effect = nullptr;
            for (usize i = 0; i < test.scene.effects().count(); ++i) {
                const auto& candidate = test.scene.effects().effect(i);
                if (candidate.tree && candidate.tree->name == head.effect) {
                    effect = &candidate;
                }
            }
            if (!effect) {
                continue;
            }
            rendered = true;
            REQUIRE(head.meshNode < stock.tree->nodes.size());
            const auto model = stock.archive.models.find(stock.tree->nodes[head.meshNode].object);
            REQUIRE(model);
            const auto& mesh = stock.archive.models.mesh(*model);
            REQUIRE_FALSE(mesh.parts.empty());
            REQUIRE_FALSE(mesh.parts.front().indices.empty());
            const auto headVertex = mesh.vertices[mesh.parts.front().indices.front()].position;
            for (const f32 alpha : {0.f, .25f, .5f, .75f, 1.f}) {
                CAPTURE(tick, alpha);
                const auto pose = fighter.presentedPose(alpha);
                const auto world = fighter.presentedNodeTransform("", alpha);
                REQUIRE(world);
                const Vec3 expected{*world * pose.matrices()[*node] * Vec4{head.throat, 1}};
                const Vec3 expectedHead{*world * pose.matrices()[head.meshNode] *
                                        Vec4{headVertex, 1}};
                test.device.draws.clear();
                test.scene.render(test.device, Mat4{1}, 640, 448, false, alpha);
                auto submitted = std::move(test.device.draws);
                test.device.draws.clear();
                // The scene selected the live texture phase on this effect's
                // model. Restrict the mouth oracle to those submitted textures,
                // rather than accepting an unrelated nearby world/body vertex.
                REQUIRE(effect->model);
                effect->model->draw(test.device, Mat4{1}, effect->transform(), {},
                                    effect->pose.matrices(), nullptr, effect->opacity());
                std::set<const Texture*> effectTextures;
                for (const auto& draw : test.device.draws) {
                    effectTextures.insert(draw.texture);
                }
                REQUIRE_FALSE(effectTextures.empty());
                bool headSubmitted = false;
                f32 nearestThroat = std::numeric_limits<f32>::max();
                for (const auto& draw : submitted) {
                    for (const auto& vertex : draw.vertices) {
                        headSubmitted |= glm::distance(vertex.position, expectedHead) < .001f;
                        if (effectTextures.contains(draw.texture)) {
                            nearestThroat =
                                std::min(nearestThroat, glm::distance(vertex.position, expected));
                        }
                    }
                }
                REQUIRE(headSubmitted); // The oracle is the actual shown head, not tick state.
                greatestSeparation = std::max(greatestSeparation, nearestThroat);
                if (effect->presentationCaptured) {
                    persistentSeparation = std::max(persistentSeparation, nearestThroat);
                }
            }
        }
        CHECK(rendered);
        CAPTURE(greatestSeparation, persistentSeparation);
        CHECK(greatestSeparation < .02f);
    }
}

TEST_CASE("Native Chimera combined breath retains the cancelling damage and effect offsets",
          "[legends][native-chimera][native-chimera-combined][assets]") {
    // Independent input oracle: e08824e8 calls e0881e34 with no world point;
    // e0881b84 adds DAMG+0x20 and SFXX+0x30 for flag0x800. Only flag1
    // discards the damage offset. The complete SUPERFIREFX hierarchy is
    // parented to the actor container's parent, not any individual head.
    const auto raw = formats::decodeLegendsN64Critter(readFile(nativeRoot() / "chi/moves.wad"));
    REQUIRE_FALSE(raw.damages.empty());
    REQUIRE(raw.sounds.size() > 6);
    REQUIRE(raw.damages[0].sfxIndex == 6);
    REQUIRE(raw.damages[0].offset == std::array<f32, 3>{0, -10, 0});
    REQUIRE(raw.sounds[6].name == "SUPERFIREFX");
    REQUIRE(raw.sounds[6].flags == 0x800);
    REQUIRE(raw.sounds[6].offset == std::array<f32, 3>{0, 10, 0});
    const auto& damageOffset = raw.damages[0].offset;
    const auto& effectOffset = raw.sounds[6].offset;
    const Vec3 nativeOffset{damageOffset[0] + effectOffset[0], damageOffset[1] + effectOffset[1],
                            damageOffset[2] + effectOffset[2]};
    REQUIRE(nativeOffset == Vec3{0});

    for (const f32 yaw : {0.f, 1.1f}) {
        CAPTURE(yaw);
        ChimeraScene test;
        for (int tick = 0; tick < 1200; ++tick) {
            test.step();
            if (!test.scene.spawning() && test.imported->opponentsReady() &&
                !test.scene.animator(0)->entering()) {
                break;
            }
        }
        REQUIRE_FALSE(test.scene.spawning());
        REQUIRE(test.imported->opponentsReady());
        REQUIRE_FALSE(test.scene.animator(0)->entering());
        test.scene.bosses().wake();
        test.forceAttack(0, "SUPER", yaw);
        usize renderedFrames = 0;
        f32 greatestSeparation = 0;
        for (int tick = 0; tick < 18; ++tick) {
            test.step();
            REQUIRE(test.scene.bosses().moveName() == "SUPER");
            const EffectTrees::Effect* effect = nullptr;
            for (usize i = 0; i < test.scene.effects().count(); ++i) {
                const auto& candidate = test.scene.effects().effect(i);
                if (candidate.tree && candidate.tree->name == "SUPERFIREFX") {
                    REQUIRE(effect == nullptr); // one compound tree, not three independent cues
                    effect = &candidate;
                }
            }
            if (!effect) {
                continue;
            }
            ++renderedFrames;
            REQUIRE(effect->tree->nodes.size() == 8);
            REQUIRE(effect->model);
            REQUIRE(effect->attachment);
            const auto& fighter = test.scene.bosses().fighter();
            for (const f32 alpha : {0.f, .25f, .5f, .75f, 1.f}) {
                CAPTURE(tick, alpha);
                const auto nativeParent = fighter.presentedNodeTransform("", alpha);
                REQUIRE(nativeParent);
                test.device.draws.clear();
                test.scene.render(test.device, Mat4{1}, 640, 448, false, alpha);
                const auto submitted = std::move(test.device.draws);
                test.device.draws.clear();
                // Reuse the live selected artwork and animation phase, but
                // independently place the complete authored mesh at the native
                // parent/offset above. Compare actual scene vertices, not just
                // a cue transform that the renderer could subsequently ignore.
                const auto& pose =
                    effect->presentationCaptured ? effect->presentationPose : effect->pose;
                effect->model->draw(test.device, Mat4{1},
                                    glm::translate(*nativeParent, nativeOffset), {},
                                    pose.matrices(), nullptr, effect->opacity());
                REQUIRE_FALSE(test.device.draws.empty());
                for (const auto& expected : test.device.draws) {
                    REQUIRE_FALSE(expected.vertices.empty());
                    for (const auto& vertex : expected.vertices) {
                        f32 nearest = std::numeric_limits<f32>::max();
                        for (const auto& actual : submitted) {
                            if (actual.texture != expected.texture) {
                                continue;
                            }
                            for (const auto& point : actual.vertices) {
                                nearest = std::min(nearest,
                                                   glm::distance(point.position, vertex.position));
                            }
                        }
                        greatestSeparation = std::max(greatestSeparation, nearest);
                    }
                }
            }
        }
        REQUIRE(renderedFrames > 10);
        CAPTURE(greatestSeparation);
        CHECK(greatestSeparation < .02f);
    }
}

TEST_CASE("Native Chimera's legend rite holds, slows and disarms nobody in its own arena",
          "[legends][native-chimera][legend][rite-hold][assets]") {
    // Boss.bin's rites run beside game.bin's ordinary player motion, which reads none of them.
    constexpr u64 kAllLegends = 0x1f;
    ChimeraScene test{kAllLegends, 2};
    auto& bosses = test.scene.bosses();
    const LegendRite& rite = bosses.legend();
    REQUIRE(rite.stage() == LegendRite::Stage::Carried);
    REQUIRE(rite.restrainsParty());
    for (s32 tick = 0; tick < 600 && (test.scene.spawning() || test.scene.animator(0)->entering());
         ++tick) {
        test.step();
    }
    const auto potions = [&test] {
        return test.scene.actor(0)->save().progress().inventory.potions.size();
    };
    // A potion used through the rite's opening goes off.
    PlayScene::Inputs input{};
    input[0].usePotion = true;
    REQUIRE(test.scene.update(1.0 / 60, input) == PlayOutcome::Running);
    input[0].usePotion = false;
    for (s32 tick = 0; tick < 120 && potions() == 2; ++tick) {
        test.step();
    }
    REQUIRE(rite.restrainsParty());
    CHECK(potions() == 1);
    for (s32 tick = 0; tick < 120 && test.scene.animator(0)->conjuring(); ++tick) {
        test.step(); // the gesture's recovery holds the feet
    }
    REQUIRE_FALSE(test.scene.animator(0)->conjuring());
    // Engaged, the party still walks at its own pace, faster than the base game's crawl allows.
    constexpr f32 kCrawlStep = 2.0f / 60.0f;
    bosses.wake();
    REQUIRE(bosses.view().engaged);
    const auto walked = [&] {
        input[0].move = {{0, 1}, 1};
        const Vec3 from = test.scene.actor(0)->position();
        REQUIRE(test.scene.update(1.0 / 60, input) == PlayOutcome::Running);
        input[0].move = {};
        const Vec3 moved = test.scene.actor(0)->position() - from;
        return std::max(std::abs(moved.x), std::abs(moved.z));
    };
    REQUIRE(rite.restrainsParty());
    CHECK(walked() > kCrawlStep + 1.0e-3f);
    // Risen, nothing holds it either.
    for (s32 tick = 0; tick < 1200 && !rite.holdsParty(); ++tick) {
        test.step();
    }
    REQUIRE(rite.holdsParty());
    CHECK(walked() > kCrawlStep + 1.0e-3f);
}
