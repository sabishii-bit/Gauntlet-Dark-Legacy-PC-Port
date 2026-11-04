#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/math/PortableMath.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "expansions/n64/bosses/LegendsBosses.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/critters/Combatant.h"
#include "game/players/PlayerImpact.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("Native Skorne eye beams damage grounded players and retain knockdown",
          "[legends][bosses][native-boss-breath][assets]") {
    const auto nativeRoot = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(nativeRoot / "sko1/moves.wad")) {
        SKIP("Native N64 boss extraction is not installed");
    }
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(nativeRoot / "_program/game.bin");
    const auto overlay = readFile(nativeRoot / "_program/Boss.bin");
    for (const char* mapName : {"boss2", "hell2"}) {
        DYNAMIC_SECTION(mapName) {
            const auto encounter = legendsN64Boss(mapName, program, overlay);
            REQUIRE(encounter);
            const auto directory = nativeRoot / "levels" / mapName;
            const auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                           readFile(directory / "objects.wad"),
                                                           readFile(directory / "ipr.rom"));
            LegendsTraversal traversal;
            traversal.open(map, 0, CharacterSave{});
            f32 floorBottom = std::numeric_limits<f32>::max();
            f32 floorTop = std::numeric_limits<f32>::lowest();
            for (const auto& face : map.collision) {
                if (!face.floor || face.normal.y < WorldCollision::kFloorNormalY)
                    continue;
                for (const auto& vertex : face.vertices) {
                    floorBottom = std::min(floorBottom, vertex.y);
                    floorTop = std::max(floorTop, vertex.y);
                }
            }
            REQUIRE(floorBottom <= floorTop);
            test::FakeRenderDevice device;
            std::vector<std::string> warnings;
            auto assets = loadLegendsBossAssets(device, nativeRoot, root, encounter->combat.kind,
                                                program, {}, warnings);
            REQUIRE(assets);
            REQUIRE(assets->tree);
            const auto move =
                std::ranges::find(assets->data.moves(), "EYEBEAM", &MoveDefinition::name);
            REQUIRE(move != assets->data.moves().end());
            const auto* damage = assets->data.damage(move->damage0);
            REQUIRE(damage);
            REQUIRE(damage->type == AttackDefinition::kBreath);
            CHECK(damage->flags == 0x23U);
            CHECK(damage->radius == 2);
            CHECK(damage->maxDistance == 50);
            CHECK(move->frameStart == 20);
            CHECK(move->frameEnd == 32);
            const auto sequenceIndex = assets->tree->findSequence(move->anim);
            REQUIRE(sequenceIndex);
            const auto& sequence = assets->tree->sequences[*sequenceIndex];
            REQUIRE(sequence.frames > move->frameEnd);
            REQUIRE(assets->tree->findNode(move->colnode, kCombatantNodeNameLength));
            Combatant actor;
            REQUIRE(actor.spawn(*assets, 0, encounter->combat.position, encounter->combat.yaw,
                                &traversal.collision(), {}, 'F'));
            Combatant::LocalSnapshot initial;
            REQUIRE(actor.saveLocal(initial));
            const Vec3 origin = actor.position();
            const f32 yaw = encounter->combat.yaw;
            const Vec3 forward{portable::sin(yaw), 0, portable::cos(yaw)};
            const Vec3 right{forward.z, 0, -forward.x};
            usize groundedProbes = 0;
            usize hits = 0;
            // Deliberately choose walking positions on the native arena floor,
            // independently of the decoded beam. Placing a target at its sampled
            // head/beam point would conceal an attack that always fires overhead.
            for (const f32 distance : {10.0f, 15.0f, 20.0f, 25.0f, 30.0f, 35.0f, 40.0f}) {
                for (const f32 sideways : {-3.0f, 0.0f, 3.0f}) {
                    Vec3 feet = origin + distance * forward + sideways * right;
                    // The native boss placement is deliberately below the arena:
                    // boss2 Y=-20, hell2 Y=-34, while the walking deck is -9.84375.
                    // Find support from the actual collision extent, independently
                    // of the actor's scripted origin and its sampled attack nodes.
                    feet.y = floorTop + 1;
                    const auto floor =
                        traversal.collision().floorAt(feet, 0, floorTop - floorBottom + 2);
                    if (!floor)
                        continue;
                    feet.y = floor->y;
                    ++groundedProbes;
                    for (s32 frame = move->frameStart - 1; frame <= move->frameEnd + 1; ++frame) {
                        REQUIRE(actor.restoreLocal(initial));
                        REQUIRE(
                            actor.setMove(static_cast<usize>(move - assets->data.moves().begin()),
                                          {.frame = static_cast<f32>(frame), .target = 0}));
                        const std::array players{EnemyView{.player = 0,
                                                           .position = feet,
                                                           .radius = 1.5f,
                                                           .height = 5,
                                                           .collisionHeight = 2.5f,
                                                           .collisionRadius = 1.5f}};
                        actor.update(1, 0, players);
                        const auto blows = actor.takeBlows();
                        CAPTURE(distance, sideways, frame, feet.x, feet.y, feet.z);
                        CHECK(actor.moveName() == "EYEBEAM");
                        if (frame < move->frameStart || frame > move->frameEnd) {
                            CHECK(blows.empty());
                            continue;
                        }
                        for (const auto& blow : blows) {
                            REQUIRE(blow.player == 0);
                            CHECK(blow.breath);
                            CHECK(blow.gated);
                            CHECK(blow.damage == Approx(damage->damage));
                            CHECK(blow.flags == 0x23U);
                            const auto reaction = PlayerImpact{blow.flags, blow.direction}.reaction(
                                blow.damage, yaw, false);
                            CHECK((reaction == PlayerDeed::FallBack ||
                                   reaction == PlayerDeed::FallForward));
                            ++hits;
                        }
                    }
                }
            }
            CHECK(groundedProbes >= 6);
            CHECK(hits > 0);
        }
    }
}
} // namespace
