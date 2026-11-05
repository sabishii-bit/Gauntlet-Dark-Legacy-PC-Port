#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"

#include "TestSupport.h"
#include "game/enemies/EnemyAnimator.h"
#include "game/enemies/EnemyKinds.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Action = EnemyAction;
constexpr f32 kStep = 1.0f / 60;

TreeInfo animatedTree() {
    TreeInfo tree;
    tree.name = "CHECKPOINT_ENEMY";
    for (s32 node = 0; node < 3; ++node) {
        TreeNodeInfo info;
        info.name = "NODE" + std::to_string(node);
        info.parent = node - 1;
        info.position = {0.125f, 0.25f * static_cast<f32>(node), -0.5f};
        tree.nodes.push_back(info);
    }
    for (const auto name : EnemyAnimator::kSequenceNames) {
        if (tree.findSequence(name)) {
            continue; // the walking and running transitions deliberately share names
        }
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 5;
        sequence.frameRate = 27;
        const auto offset = static_cast<f32>(tree.sequences.size()) * 0.02f;
        for (u32 node = 0; node < tree.nodes.size(); ++node) {
            TrackInfo track;
            track.node = node;
            track.flags = static_cast<u16>(TrackInfo::kChannels |
                                           ((node % 2) != 0 ? TrackInfo::kPitchYawRoll : 0));
            track.frames = {0, 4};
            track.values = {offset, 0.2f,         -0.3f, offset, 0.5f,         0.75f, 1,    1.125f,
                            0.875f, offset + .4f, 0.6f,  -0.1f,  offset + .5f, 0.6f,  0.8f, .9f,
                            1,      1.1f};
            sequence.trackOfNode.push_back(static_cast<s32>(sequence.tracks.size()));
            sequence.tracks.push_back(track);
        }
        tree.sequences.push_back(sequence);
    }
    return tree;
}

void samePose(const TreePose& actual, const TreePose& expected) {
    CHECK(std::ranges::equal(actual.poses(), expected.poses()));
    CHECK(std::ranges::equal(actual.matrices(), expected.matrices()));
}

void sameAnimator(const EnemyAnimator& actual, const EnemyAnimator& expected) {
    CHECK(actual.equalLocal(expected));
    samePose(actual.pose(), expected.pose());
    for (const f32 fraction : {0.0f, 0.375f, 1.0f}) {
        TreePose actualPose;
        TreePose expectedPose;
        actual.evaluatePresentation(actualPose, fraction);
        expected.evaluatePresentation(expectedPose, fraction);
        samePose(actualPose, expectedPose);
        CHECK(actual.presentationFrame(fraction) == expected.presentationFrame(fraction));
    }
}

// The swarm rewinds a body by copying it whole (Enemies' local snapshot copies each
// slot's animator), so these replays copy the animator and play on from the copy.

TEST_CASE("enemy animator rewind retains pending actions release latches and exact native poses",
          "[rollback][enemies][enemy-animator-local-rewind]") {
    const auto tree = animatedTree();
    std::array<bool, kEnemyActionCount> requested{};
    bool struck = false;
    bool powerStruck = false;
    bool threw = false;
    bool dead = false;
    for (usize action = 0; action < kEnemyActionCount; ++action) {
        CAPTURE(action);
        EnemyAnimator native;
        EnemyAnimator replay;
        REQUIRE(native.bind(tree, action % 2 != 0));
        REQUIRE(replay.bind(tree, action % 2 == 0));
        native.setThrowInterval(0.25f);
        for (s32 tick = 0; tick < 100; ++tick) {
            CAPTURE(tick);
            const auto next = static_cast<Action>(action);
            native.request(next);
            requested[static_cast<usize>(native.requested())] = true;
            replay = native;
            sameAnimator(replay, native);
            const f32 seconds = tick % 7 == 0 ? kStep * 0.25f : kStep;
            const bool contact = tick % 5 < 3;
            native.update(1, seconds, contact);
            replay.update(1, seconds, contact);
            sameAnimator(replay, native);
            struck = struck || native.struck();
            powerStruck = powerStruck || native.powerStruck();
            threw = threw || native.threw();
            dead = dead || native.dead();
            replay = native;
            sameAnimator(replay, native);
        }
    }
    CHECK(std::ranges::all_of(requested, [](bool value) { return value; }));
    CHECK(struck);
    CHECK(powerStruck);
    CHECK(threw);
    CHECK(dead);
}

TEST_CASE("enemy animator rewind replays repeated cuts and carries a throw's wait",
          "[rollback][enemies][enemy-animator-local-rewind]") {
    auto tree = animatedTree();
    std::erase_if(tree.sequences, [](const auto& sequence) { return sequence.name == "DEATH"; });
    EnemyAnimator native;
    REQUIRE(native.bind(tree));
    native.setThrowInterval(1);
    std::vector<EnemyAnimator> history;
    std::vector<Action> inputs;
    bool waited = false;
    bool sawDeathFallback = false;
    for (s32 tick = 0; tick < 250; ++tick) {
        CAPTURE(tick);
        history.push_back(native);
        Action action = Action::Throw;
        if (tick >= 210) {
            action = Action::Dying;
        } else if (tick == 110 || tick == 150) {
            action = Action::HitReact1;
        }
        inputs.push_back(action);
        native.request(action);
        native.update(1, kStep, true);
        waited = waited || native.idleSeconds() > 1;
        if (native.dying()) {
            sawDeathFallback = true;
            CHECK(native.player().sequence() == native.sequenceOf(Action::HitReact2));
        }
        const auto start = static_cast<usize>(std::max(tick - 9, 0));
        EnemyAnimator replay = history[start];
        for (usize at = start; at < inputs.size(); ++at) {
            replay.request(inputs[at]);
            replay.update(1, kStep, true);
        }
        sameAnimator(replay, native);
    }
    CHECK(waited);
    CHECK(sawDeathFallback);
    CHECK(native.dead());
}

TEST_CASE("enemy animator rewind restores held presentation",
          "[rollback][enemies][enemy-animator-local-rewind]") {
    const auto tree = animatedTree();
    EnemyAnimator native;
    REQUIRE(native.bind(tree));
    for (s32 tick = 0; tick < 30; ++tick) {
        native.request(Action::Walk);
        native.update(1, kStep);
    }
    EnemyAnimator replay = native;
    sameAnimator(replay, native);
    for (s32 tick = 0; tick < 30; ++tick) {
        native.request(Action::Walk);
        replay.request(Action::Walk);
        native.update(1, kStep);
        replay.update(1, kStep);
        sameAnimator(replay, native);
    }
    native.holdPresentation();
    replay = native;
    sameAnimator(replay, native);
    TreePose held;
    replay.evaluatePresentation(held, 0);
    TreePose complete;
    replay.evaluatePresentation(complete, 1);
    samePose(held, complete);
}

TEST_CASE("the invisible IT body rewinds without inventing a skeleton",
          "[rollback][enemies][enemy-animator-local-rewind]") {
    TreeInfo tree;
    tree.name = "IT";
    TreeSequenceInfo ready;
    ready.name = "READY";
    ready.frames = 1;
    ready.frameRate = 30;
    ready.repeats = true;
    tree.sequences.push_back(ready);
    EnemyAnimator native;
    EnemyAnimator replay;
    REQUIRE(native.bind(tree));
    REQUIRE(replay.bind(tree));
    for (s32 tick = 0; tick < 40; ++tick) {
        native.request(tick < 20 ? Action::Walk : Action::Dying);
        CHECK(native.pose().poses().empty());
        replay = native;
        native.update(1, kStep);
        replay.update(1, kStep);
        sameAnimator(replay, native);
    }
    CHECK(native.dead());
}

TEST_CASE("native swarm families resume all body state from delayed copies",
          "[rollback][enemies][enemy-animator-local-rewind][assets]") {
    const auto monsters = test::assetOrSkip("MONSTERS");
    s32 families = 0;
    s32 bodies = 0;
    for (s32 kind = 0; kind <= kDeathKind; ++kind) {
        if (kind >= kSwarmKindCount && kind != kDeathKind) {
            continue;
        }
        const auto& info = enemyKind(kind);
        CAPTURE(info.name);
        AnimationSet assets;
        REQUIRE(assets.load(monsters / std::string(info.name)));
        s32 found = 0;
        for (const auto* const suffix : {"1", "2", "3", "A", "B", "S"}) {
            const auto treeIndex = assets.find(std::string(info.prefix) + suffix);
            if (!treeIndex) {
                continue; // native families deliberately omit unsupported tiers/variants
            }
            const auto& tree = assets.tree(*treeIndex);
            CAPTURE(tree.name);
            EnemyAnimator native;
            REQUIRE(native.bind(tree, (kind % 2) != 0));
            native.setThrowInterval(0.25f);
            std::vector<EnemyAnimator> history;
            std::vector<Action> inputs;
            for (s32 tick = 0; tick < 160; ++tick) {
                CAPTURE(tick);
                history.push_back(native);
                constexpr std::array kActions{Action::Walk,  Action::Attack,    Action::PowerAttack,
                                              Action::Throw, Action::RunAttack, Action::HitReact2,
                                              Action::Walk,  Action::Dying};
                const auto action = kActions[static_cast<usize>(tick / 20)];
                inputs.push_back(action);
                native.request(action);
                native.update(2, kStep * 2, tick % 3 != 0);
                if (tick % 8 != 0 && tick != 159) {
                    continue;
                }
                const auto start = static_cast<usize>(std::max(tick - 5, 0));
                EnemyAnimator replay = history[start];
                for (usize at = start; at < inputs.size(); ++at) {
                    replay.request(inputs[at]);
                    replay.update(2, kStep * 2, at % 3 != 0);
                }
                sameAnimator(replay, native);
            }
            ++found;
            ++bodies;
        }
        REQUIRE(found > 0);
        ++families;
    }
    CHECK(families == kSwarmKindCount + 1);
    CHECK(bodies >= families);
}
} // namespace
