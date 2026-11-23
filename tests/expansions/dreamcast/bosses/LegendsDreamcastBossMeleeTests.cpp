#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/ApproximateLength.h"
#include "engine/math/Math.h"
#include "engine/math/PortableMath.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBossAssets.h"
#include "game/critters/Combatant.h"
#include "game/critters/CombatantAssets.h"
#include "game/level/BossKinds.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr f32 kNativeFrame = 1.0f / 30; ///< the original's frame, over which a blow's swing runs
constexpr s32 kTicksPerFrame = 2;
constexpr s32 kMostFrames = 120;
constexpr f32 kContactRadius = 1.5f;
constexpr f32 kContactHeight = 2.5f;
constexpr f32 kHeight = 5.0f;
constexpr f32 kStill = 1e-3f;
constexpr f32 kTolerance = 1e-3f;
const Vec3 kSpawn{12, 3, -9};
constexpr f32 kSpawnYaw = 0.4f;

std::filesystem::path nativeRoot() {
    return test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
}

/** A blow's push as 1ST_READ.BIN's CritterNodePlayerCollide (8c08d4c0) builds it: twice the sum
 * of the node's travel over the native frame and the way from the body's origin to the feet,
 * that given a unit's height, each scaled by the reciprocal of its fast length (8c0ec8e0). */
Vec3 swingPush(const Vec3& before, const Vec3& after, const Vec3& body, const Vec3& feet) {
    return 2.0f * (approximateUnit(after - before) +
                   approximateUnit(Vec3{feet.x - body.x, 1.0f, feet.z - body.z}));
}

/** The body's origin a blow pushes away from: the model's root with the TYPE origin turned with
 * it (c+0x4C, ProcessCritter 8c08b460). */
Vec3 bodyOrigin(const Combatant& body) {
    const Vec3 root = body.position() + Vec3{0, body.data()->floorOffset(), 0};
    const Mat4 model =
        glm::scale(portable::rotate(glm::translate(Mat4{1}, root), body.yaw(), Vec3{0, 1, 0}),
                   Vec3{body.scale() * body.shrink()});
    return Vec3{model * Vec4{body.data()->originOffset(), 1}};
}

/** A player standing half `reach` beside `centre`, across `swing`, so the swing and the way
 * from the body point different ways; its contact centre stands level with `centre`. */
std::array<EnemyView, 1> besideSwing(const Vec3& centre, const Vec3& swing, const Vec3& body,
                                     f32 reach) {
    Vec2 along{swing.x, swing.z};
    if (glm::length(along) < kStill) {
        along = Vec2{centre.x - body.x, centre.z - body.z};
    }
    along = glm::length(along) < kStill ? Vec2{1, 0} : glm::normalize(along);
    const Vec2 across = 0.5f * reach * Vec2{-along.y, along.x};
    return {
        EnemyView{.player = 0,
                  .position = {centre.x + across.x, centre.y - kContactHeight, centre.z + across.y},
                  .radius = 0.5f,
                  .height = kHeight,
                  .collisionHeight = kContactHeight,
                  .collisionRadius = kContactRadius}};
}

/** Where move `index`'s node stands at `frame` from the body as `state` left it: what the move's
 * setup notes each native frame (MoveSetup 8c08a2c0), after the animation and before the step
 * and the turn. */
Mat4 notedNode(Combatant& actor, const Combatant::LocalSnapshot& state, usize index, f32 frame) {
    REQUIRE(actor.restoreLocal(state));
    REQUIRE(actor.setMove(index, {.frame = frame, .target = 0}));
    const auto node = actor.nodeTransform(actor.data()->moves()[index].colnode);
    REQUIRE(node);
    return *node;
}

/** What an update did: its blows, and where the move's setup noted the node and stood the body's
 * origin. */
struct Noted {
    std::vector<CombatBlow> blows;
    Vec3 node{0};
    Vec3 body{0};
    f32 frame = 0;
};

/** The move's type-0 blow, or null when it has none that harms or no node to strike with. */
const AttackDefinition* blowOf(const CritterData& data, const MoveDefinition& move) {
    const AttackDefinition* damage = data.damage(move.damage0);
    return damage != nullptr && damage->type == AttackDefinition::kBlow && damage->damage > 0 &&
                   !move.colnode.empty()
               ? damage
               : nullptr;
}

TEST_CASE("a Dreamcast boss's blow pushes along its striking part's swing and away from its body",
          "[legends][bosses][native-boss-melee][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "critters/dragon.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    struct Bank {
        s32 kind;
        usize blows;
    };
    // The bodies' blows: the dragon's two claws, the djinn's seven, the drider's two charges and
    // each Skorne's left claw. The chimera's are its heads', below.
    constexpr std::array kBanks{Bank{boss_type::kDragon, 2}, Bank{boss_type::kDjinn, 7},
                                Bank{boss_type::kDrider, 2}, Bank{boss_type::kSkorne1, 1},
                                Bank{boss_type::kSkorne2, 1}};
    for (const auto& bank : kBanks) {
        DYNAMIC_SECTION("Dreamcast boss " << bank.kind) {
            test::FakeRenderDevice device;
            std::vector<std::string> warnings;
            auto stock = loadLegendsDreamcastBossAssets(device, root, {}, bank.kind, {}, warnings);
            REQUIRE(stock);
            // Standing at home, the territory's clamp never moves it.
            const f32 floorOffset = stock->data.floorOffset();
            const Vec3 home =
                stock->data.movement().home.value_or(kSpawn + Vec3{0, floorOffset, 0}) -
                Vec3{0, floorOffset, 0};
            Combatant actor;
            REQUIRE(actor.spawn(*stock, 0, home, kSpawnYaw, nullptr, {}, 'A'));
            Combatant::LocalSnapshot spawned;
            REQUIRE(actor.saveLocal(spawned));
            usize tested = 0;
            for (usize index = 0; index < stock->data.moves().size(); ++index) {
                const auto& move = stock->data.moves()[index];
                const AttackDefinition* damage = blowOf(stock->data, move);
                if (damage == nullptr) {
                    continue;
                }
                ++tested;
                CAPTURE(move.name, move.colnode, move.frameStart, move.frameEnd);
                const s32 last = std::max(move.frameStart, move.frameEnd);
                const auto opening = static_cast<f32>(std::max(move.frameStart, 0));
                const Mat4 strikes = notedNode(actor, spawned, index, opening);
                const Vec3 swing =
                    Vec3{notedNode(actor, spawned, index, opening + 1)[3]} - Vec3{strikes[3]};
                REQUIRE(actor.restoreLocal(spawned));
                const auto players = besideSwing(Vec3{strikes * Vec4{damage->offset, 1}}, swing,
                                                 bodyOrigin(actor), damage->maxDistance);

                // The body steps and turns after its strikes, so each update's noted node is
                // taken from where it stood as the update began.
                const auto advance = [&](s32 ticks, f32 seconds,
                                         std::span<const EnemyView> roster) {
                    Combatant::LocalSnapshot before;
                    REQUIRE(actor.saveLocal(before));
                    Noted out;
                    out.body = bodyOrigin(actor);
                    actor.update(ticks, seconds, roster);
                    out.blows = actor.takeBlows();
                    out.frame = actor.animation().frame();
                    Combatant::LocalSnapshot after;
                    REQUIRE(actor.saveLocal(after));
                    out.node = Vec3{notedNode(actor, before, index, out.frame)[3]};
                    REQUIRE(actor.restoreLocal(after));
                    return out;
                };
                REQUIRE(actor.setMove(
                    index,
                    {.frame = static_cast<f32>(std::max(move.frameStart - 1, 0)), .target = 0}));
                // An update of no time notes the node without moving it: the sample the first
                // native frame measures from.
                const Noted primed = advance(1, 0.0f, {});
                REQUIRE(primed.blows.empty());
                Vec3 nodeBefore = primed.node;
                std::optional<Noted> landing;
                for (s32 frame = 0; frame < kMostFrames && !landing &&
                                    std::floor(actor.animation().frame()) <= static_cast<f32>(last);
                     ++frame) {
                    Noted step = advance(kTicksPerFrame, kNativeFrame, players);
                    if (step.blows.empty()) {
                        nodeBefore = step.node;
                    } else {
                        landing = std::move(step);
                    }
                }
                REQUIRE(landing);
                CHECK(std::floor(landing->frame) >= static_cast<f32>(move.frameStart));
                REQUIRE(landing->blows.size() == 1);
                CHECK(landing->blows[0].player == 0);
                const Vec3 expected =
                    swingPush(nodeBefore, landing->node, landing->body, players[0].position);
                const Vec3& direction = landing->blows[0].direction;
                CAPTURE(direction.x, direction.y, direction.z, expected.x, expected.y, expected.z);
                CHECK(glm::distance(direction, expected) < kTolerance);
            }
            CHECK(tested == bank.blows);
        }
    }
}

TEST_CASE("a Dreamcast Chimera head's bite pushes along its head's swing and away from the body",
          "[legends][bosses][native-boss-melee][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "critters/chimera.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    auto stock =
        loadLegendsDreamcastBossAssets(device, root, {}, boss_type::kChimera, {}, warnings);
    REQUIRE(stock);
    Combatant actor;
    REQUIRE(actor.spawn(*stock, 0, kSpawn, kSpawnYaw, nullptr, {}, 'A'));
    REQUIRE(actor.childCount() == 3);
    Combatant::LocalSnapshot spawned;
    REQUIRE(actor.saveLocal(spawned));
    const Vec3 body = bodyOrigin(actor);
    // The eagle's bite, the lion's two and the snake's.
    constexpr std::array<usize, 3> kBites{1, 2, 1};
    for (s32 id = 1; id <= static_cast<s32>(actor.childCount()); ++id) {
        DYNAMIC_SECTION("head " << id) {
            const auto head = [&] {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
                auto* part = const_cast<Combatant*>(actor.child(id));
                REQUIRE(part);
                return part;
            };
            const CritterData& data = *head()->data();
            usize tested = 0;
            for (usize index = 0; index < data.moves().size(); ++index) {
                const auto& move = data.moves()[index];
                const AttackDefinition* damage = blowOf(data, move);
                if (damage == nullptr) {
                    continue;
                }
                ++tested;
                CAPTURE(move.name, move.colnode, move.frameStart, move.frameEnd);
                REQUIRE(move.frameStart > 0);
                // Driven by itself, a head neither steps nor turns, so where an update leaves its
                // node is where its setup noted it, over the body's pose.
                const auto node = [&] {
                    const auto transform = head()->nodeTransform(move.colnode);
                    REQUIRE(transform);
                    return *transform;
                };
                const auto posedAt = [&](f32 frame) {
                    REQUIRE(actor.restoreLocal(spawned));
                    REQUIRE(head()->setMove(index, {.frame = frame, .target = 0}));
                    head()->update(1, 0.0f, {});
                    return node();
                };
                const auto opening = static_cast<f32>(move.frameStart);
                const Mat4 strikes = posedAt(opening);
                const Vec3 swing = Vec3{posedAt(opening + 1)[3]} - Vec3{strikes[3]};
                const auto players = besideSwing(Vec3{strikes * Vec4{damage->offset, 1}}, swing,
                                                 body, damage->maxDistance);
                REQUIRE(actor.restoreLocal(spawned));
                REQUIRE(head()->setMove(
                    index, {.frame = static_cast<f32>(move.frameStart - 1), .target = 0}));
                head()->update(1, 0.0f, {});
                REQUIRE(head()->takeBlows().empty());
                const s32 last = std::max(move.frameStart, move.frameEnd);
                Vec3 nodeBefore{0};
                std::vector<CombatBlow> blows;
                for (s32 frame = 0;
                     frame < kMostFrames && blows.empty() &&
                     std::floor(head()->animation().frame()) <= static_cast<f32>(last);
                     ++frame) {
                    nodeBefore = Vec3{node()[3]};
                    head()->update(kTicksPerFrame, kNativeFrame, players);
                    blows = head()->takeBlows();
                }
                REQUIRE(blows.size() == 1);
                CHECK(blows[0].player == 0);
                CHECK(std::floor(head()->animation().frame()) >= static_cast<f32>(move.frameStart));
                const Vec3 expected =
                    swingPush(nodeBefore, Vec3{node()[3]}, bodyOrigin(actor), players[0].position);
                const Vec3& direction = blows[0].direction;
                CAPTURE(direction.x, direction.y, direction.z, expected.x, expected.y, expected.z);
                CHECK(glm::distance(direction, expected) < kTolerance);
            }
            CHECK(tested == kBites[static_cast<usize>(id - 1)]);
        }
    }
}
} // namespace
