#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/netplay/CombatPlayback.h"
#include "game/netplay/ReplicaPose.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

CombatSnapshot state(u64 tick) {
    CombatSnapshot result;
    result.motion.epoch = 1;
    result.motion.tick = tick;
    result.motion.cameraContinuity = 1;
    result.motion.players[0] = SeatMotion{1, 1, {0, 0, 0}, 0};
    result.players[0] =
        PlayerCombatState{1000, ReplicaPlayerLife::Standing, false, true, {4, 1, 1, 0, 1}};
    EnemyCombatState enemy;
    enemy.instance = 7;
    enemy.health = 40;
    enemy.fullHealth = 50;
    enemy.animation = {2, 1, 1, 0, 1};
    result.enemies.push_back(enemy);
    return result;
}
void receive(CombatPlayback& playback, const CombatSnapshot& state) {
    const auto packets = CombatReplica::packets(state);
    REQUIRE(packets);
    for (usize i = 0; i < packets->size(); ++i) {
        REQUIRE(playback.receive(1, (*packets)[i]) == (i + 1 == packets->size()
                                                           ? CombatReplica::Admission::Committed
                                                           : CombatReplica::Admission::Pending));
    }
}
TEST_CASE("static fixture terminal frames remain exactly in range at every sample fraction",
          "[netplay][combat-playback]") {
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    auto first = state(0);
    FixtureState fixture;
    fixture.instance = fixture.resource = 1;
    fixture.pose = {0, 0, 1, 29, 1};
    fixture.meshFrame = fixture.textureFrame = 29;
    first.fixtures.push_back(fixture);
    auto last = first;
    last.motion.tick = 3;
    receive(playback, first);
    receive(playback, last);
    for (s32 step = 0; step < 1000; ++step) {
        const auto shown = playback.sample(1, static_cast<f32>(step) / 1000);
        REQUIRE(shown);
        REQUIRE(shown->fixtures.size() == 1);
        CHECK(shown->fixtures[0].pose.frame == 29);
        CHECK(shown->fixtures[0].meshFrame == 29);
        CHECK(shown->fixtures[0].textureFrame == 29);
    }
}

TEST_CASE(
    "player shadows follow the snapshot clock on slopes and moving floors without crossing cuts",
    "[netplay][combat-playback][replica-shadows]") {
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    auto first = state(10);
    first.players[0]->shadow = PlayerShadowState{{0, 3, 0}, {0, 1, 0}, 1};
    auto last = first;
    last.motion.tick = 14;
    last.motion.players[0]->position = {8, 7, 0};
    last.players[0]->shadow = PlayerShadowState{{8, 7, 0}, {0.6f, 0.8f, 0}, 0.5f};
    bool continuous = true;
    SECTION("moving floor interpolates with the body") {}
    SECTION("teleport cuts do not smear a shadow between floors") {
        ++last.motion.players[0]->continuity;
        continuous = false;
    }
    SECTION("a new seat owner does not inherit the previous shadow") {
        ++last.motion.players[0]->grant;
        continuous = false;
    }
    SECTION("leaving a floor retains the last contact until its checkpoint") {
        last.players[0]->shadow.reset();
        continuous = false;
    }
    receive(playback, first);
    receive(playback, last);
    const auto shown = playback.sample(12);
    REQUIRE(shown);
    REQUIRE(shown->players[0]->shadow);
    const auto& shadow = *shown->players[0]->shadow;
    CHECK(shadow.ground == (continuous ? Vec3{4, 5, 0} : Vec3{0, 3, 0}));
    CHECK(shadow.alpha == Approx(continuous ? 0.75f : 1));
    CHECK(glm::length(shadow.normal) == Approx(1));
    CHECK(shown->valid());
    CHECK(CombatPacket::encode(*playback.sample(14)) == CombatPacket::encode(last));
}
TEST_CASE("combat playback samples camera and actor poses on one delayed clock",
          "[netplay][combat-playback]") {
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    auto first = state(10);
    first.enemies[0].yaw = 3.1f;
    auto last = first;
    last.motion.tick = 14;
    last.motion.camera.position.x = 8;
    last.motion.players[0]->position.x = 8;
    last.players[0]->animation.frame = 4;
    last.players[0]->health = 900;
    last.players[0]->hitFlash = true;
    last.enemies[0].position.x = 8;
    last.enemies[0].animation.frame = 4;
    last.enemies[0].health = 30;
    last.enemies[0].yaw = -3.1f;
    receive(playback, first);
    receive(playback, last);
    const auto shown = playback.sample(11, 0.5f);
    REQUIRE(shown);
    CHECK(shown->motion.camera.position.x == Approx(3));
    CHECK(shown->motion.players[0]->position.x == Approx(3));
    CHECK(shown->players[0]->animation.frame == Approx(1.5f));
    CHECK(shown->players[0]->health == 1000);
    CHECK_FALSE(shown->players[0]->hitFlash);
    CHECK(shown->enemies[0].position.x == Approx(3));
    CHECK(shown->enemies[0].animation.frame == Approx(1.5f));
    CHECK(shown->enemies[0].health == 40);
    CHECK(std::abs(shown->enemies[0].yaw) > 3);
    CHECK(CombatPacket::encode(*playback.sample(0)) == CombatPacket::encode(first));
    CHECK(CombatPacket::encode(*playback.sample(10000)) == CombatPacket::encode(last));
    CHECK(playback.sample(14)->players[0]->health == 900);
    CHECK(playback.sample(14)->players[0]->hitFlash);
}

TEST_CASE("portal playback interpolates only a committed departure on the shared clock",
          "[netplay][combat-playback][online-departure]") {
    auto first = state(10);
    first.players[0]->portalPhase = 0.2f;
    auto last = first;
    last.motion.tick = 20;
    last.players[0]->portalPhase = 0.4f;
    SECTION("continuous departure at different render rates") {
        CombatPlayback playback;
        REQUIRE(playback.begin(1, 1));
        receive(playback, first);
        receive(playback, last);
        for (const s32 fps : {30, 60, 144}) {
            for (s32 frame = 0; frame <= fps / 6; ++frame) {
                const f64 time = 10 + static_cast<f64>(frame) * 60 / fps;
                const auto tick = static_cast<u64>(time);
                const auto sample =
                    playback.sample(tick, static_cast<f32>(time - static_cast<f64>(tick)));
                REQUIRE(sample);
                REQUIRE(sample->players[0]->portalPhase);
                CHECK(*sample->players[0]->portalPhase == Approx(time * 0.02));
                CHECK(sample->motion.players[0]->position == first.motion.players[0]->position);
            }
        }
        CHECK(playback.sample(100)->players[0]->portalPhase == 0.4f); // no autonomous countdown
        REQUIRE(playback.begin(1, 2));
        CHECK_FALSE(playback.sample(10));
    }
    SECTION("not before commitment") {
        first.players[0]->portalPhase.reset();
    }
    SECTION("not through a seat replacement") {
        ++last.motion.players[0]->grant;
    }
    SECTION("not through a relocation") {
        ++last.motion.players[0]->continuity;
    }
    SECTION("not backwards into a new departure") {
        last.players[0]->portalPhase = 0.0f;
    }
    SECTION("not into a death") {
        last.players[0]->life = ReplicaPlayerLife::Dying;
        last.players[0]->portalPhase.reset();
    }
    if (!first.players[0]->portalPhase || last.players[0]->portalPhase != 0.4f ||
        last.motion.players[0]->grant != 1 || last.motion.players[0]->continuity != 1) {
        CombatPlayback playback;
        REQUIRE(playback.begin(1, 1));
        receive(playback, first);
        receive(playback, last);
        REQUIRE(playback.sample(15));
        CHECK(playback.sample(15)->players[0]->portalPhase == first.players[0]->portalPhase);
        CHECK(playback.sample(20)->players[0]->portalPhase == last.players[0]->portalPhase);
    }
}

TEST_CASE(
    "fixture playback smooths carried poses but holds visibility resources and empty state cuts",
    "[netplay][combat-playback]") {
    auto first = state(10);
    for (u64 id = 1; id <= 6; ++id) {
        FixtureState fixture;
        fixture.instance = id;
        fixture.resource = 1;
        fixture.pose = {0, 1, 2, 4, 1};
        fixture.meshSequence = fixture.textureSequence = 1;
        fixture.meshFrame = fixture.textureFrame = 4;
        first.fixtures.push_back(fixture);
    }
    auto last = first;
    last.motion.tick = 14;
    for (auto& fixture : last.fixtures) {
        fixture.placement[3].y = -8;
        fixture.pose.frame = fixture.meshFrame = fixture.textureFrame = 8;
        fixture.textureClock = 8;
    }
    last.fixtures[1].meshSequence = 2;
    last.fixtures[1].meshFrame = 0;
    last.fixtures[1].pose = first.fixtures[1].pose;
    last.fixtures[1].textureFrame = 4;
    last.fixtures[2].resource = 2;
    ++last.fixtures[3].continuity;
    ++last.fixtures[4].pose.generation;
    last.fixtures.back().instance = 7;
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    receive(playback, first);
    receive(playback, last);
    const auto middle = playback.sample(12);
    REQUIRE(middle);
    REQUIRE(middle->fixtures.size() == 6);
    CHECK(middle->fixtures[0].placement[3].y == Approx(-4));
    CHECK(middle->fixtures[0].pose.frame == Approx(6));
    CHECK(middle->fixtures[0].meshFrame == Approx(6));
    CHECK(middle->fixtures[0].textureFrame == Approx(6));
    CHECK(middle->fixtures[0].textureClock == Approx(4));
    CHECK(middle->fixtures[1].pose.frame == 4);
    CHECK(middle->fixtures[1].meshSequence == 1);
    CHECK(middle->fixtures[2].resource == 1);
    CHECK(middle->fixtures[2].placement[3].y == 0);
    CHECK(middle->fixtures[3].placement[3].y == 0);
    CHECK(middle->fixtures[4].pose.frame == 4);
    CHECK(middle->fixtures[4].meshFrame == 4);
    CHECK(middle->fixtures.back().instance == 6);
    CHECK(CombatPacket::encode(*playback.sample(14)) == CombatPacket::encode(last));
}

TEST_CASE("geometry playback interpolates local arcs and holds visibility and placement cuts",
          "[netplay][combat-playback][geometry-replica]") {
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    auto first = state(10);
    first.geometry = SceneGeometry{1, 3, 0, {{0}, {1}, {2}}};
    first.geometry->objects[1].local = glm::translate(Mat4{1}, Vec3{10, 0, 0});
    first.geometry->objects[2].local = glm::scale(
        glm::rotate(Mat4{1}, 0.71f, glm::normalize(Vec3{1, 2, 3})), Vec3{0.3f, 0.7f, 1.2f});
    auto last = first;
    last.motion.tick = 14;
    last.motion.players[0]->position.y = 8;
    last.geometry->darken = 1;
    last.geometry->objects[0].local =
        glm::rotate(glm::translate(Mat4{1}, Vec3{0, 8, 0}), kHalfPi, Vec3{0, 1, 0});
    last.geometry->objects[1].alpha = 0;
    last.geometry->objects[2].visible = false;
    SECTION("continuous motion") {
        receive(playback, first);
        receive(playback, last);
        const auto shown = playback.sample(12);
        REQUIRE(shown);
        REQUIRE(shown->geometry);
        const auto& geometry = *shown->geometry;
        CHECK(geometry.darken == 0.5f);
        const Mat4 child = geometry.objects[0].local * geometry.objects[1].local;
        CHECK(glm::length(Vec2{child[3].x, child[3].z}) == Approx(10)); // arc, not chord
        CHECK(child[3].y == Approx(4));
        CHECK(child[3].y == shown->motion.players[0]->position.y);
        CHECK(geometry.objects[1].alpha == 0.5f);
        CHECK(geometry.objects[2].visible);
        CHECK(geometry.objects[2].local == first.geometry->objects[2].local);
        CHECK_FALSE(playback.sample(14)->geometry->objects[2].visible);
    }
    SECTION("placement cut") {
        ++last.geometry->objects[0].continuity;
        receive(playback, first);
        receive(playback, last);
        CHECK(playback.sample(12)->geometry->objects[0].local == Mat4{1});
        CHECK(playback.sample(14)->geometry->objects[0].local == last.geometry->objects[0].local);
    }
    SECTION("different layout") {
        ++last.geometry->layout;
        receive(playback, first);
        receive(playback, last);
        CHECK(playback.sample(12)->geometry->objects[0].local == Mat4{1});
        CHECK(playback.sample(12)->geometry->darken == 0);
    }
}

TEST_CASE("combat display never blends actions wraps death or replacement identities",
          "[netplay][combat-playback]") {
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    auto first = state(10);
    auto last = first;
    last.motion.tick = 20;
    last.players[0]->animation.frame = 8;
    last.enemies[0].animation.frame = 8;
    SECTION("animation restart") {
        ++last.players[0]->animation.generation;
        ++last.enemies[0].animation.generation;
    }
    SECTION("action change") {
        ++last.players[0]->animation.action;
        ++last.enemies[0].animation.action;
    }
    SECTION("sequence change") {
        ++last.players[0]->animation.sequence;
        ++last.enemies[0].animation.sequence;
    }
    SECTION("frame wrap even when generation did not change") {
        first.players[0]->animation.frame = 9;
        first.enemies[0].animation.frame = 9;
    }
    SECTION("life change") {
        last.players[0]->life = ReplicaPlayerLife::Dying;
        last.enemies[0].life = ReplicaEnemyLife::Dying;
    }
    SECTION("actor replacement") {
        ++last.motion.players[0]->grant;
        ++last.enemies[0].instance;
    }
    SECTION("teleport and different enemy appearance") {
        ++last.motion.players[0]->continuity;
        ++last.enemies[0].variant;
    }
    SECTION("actor removal") {
        last.motion.players[0].reset();
        last.players[0].reset();
        last.enemies.clear();
    }
    receive(playback, first);
    receive(playback, last);
    const auto middle = playback.sample(19, 0.9f);
    REQUIRE(middle);
    CHECK(middle->players[0]->animation.frame == first.players[0]->animation.frame);
    CHECK(middle->enemies[0].animation.frame == first.enemies[0].animation.frame);
    CHECK(middle->enemies[0].instance == 7);
    CHECK(CombatPacket::encode(*playback.sample(20)) == CombatPacket::encode(last));
}

TEST_CASE("incomplete or untrusted combat packets never alter the displayed scene",
          "[netplay][combat-playback]") {
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    const auto first = state(10);
    receive(playback, first);
    auto next = state(20);
    next.enemies.clear();
    for (u64 id = 1; id <= 25; ++id) {
        auto enemy = first.enemies[0];
        enemy.instance = id;
        next.enemies.push_back(enemy);
    }
    next.players[0]->health = 500;
    const auto packets = CombatReplica::packets(next, SnapshotBlock::Compression::None);
    REQUIRE(packets);
    REQUIRE(packets->size() > 1);
    CHECK(playback.receive(2, packets->front()) == CombatReplica::Admission::WrongHost);
    CHECK(playback.receive(1, packets->back()) == CombatReplica::Admission::Pending);
    CHECK(playback.receive(1, packets->back()) == CombatReplica::Admission::Duplicate);
    CHECK(playback.size() == 1);
    CHECK(CombatPacket::encode(*playback.sample(20)) == CombatPacket::encode(first));
    for (usize i = 0; i + 1 < packets->size(); ++i) {
        playback.receive(1, (*packets)[i]);
    }
    CHECK(playback.size() == 2);
    CHECK(playback.sample(19)->enemies.size() == 1);
    CHECK(CombatPacket::encode(*playback.sample(20)) == CombatPacket::encode(next));
}

TEST_CASE("combat presentation history is bounded and resets with its trusted epoch",
          "[netplay][combat-playback]") {
    CombatPlayback playback;
    CHECK_FALSE(playback.sample(0));
    CHECK_FALSE(playback.begin(0, 1));
    REQUIRE(playback.begin(1, 1));
    constexpr u64 kStart = std::numeric_limits<u64>::max() - 1000;
    for (u64 i = 0; i < 100; ++i) {
        auto next = state(kStart + i * 2);
        next.enemies[0].position.x = static_cast<f32>(i * 2);
        receive(playback, next);
        CHECK(playback.size() <= SnapshotPlayback::kHistory);
    }
    CHECK(playback.size() == SnapshotPlayback::kHistory);
    CHECK(playback.sample(kStart + 195, 0.5f)->enemies[0].position.x == Approx(195.5f));
    CHECK_FALSE(playback.sample(0, -1));
    CHECK_FALSE(playback.sample(0, std::numeric_limits<f32>::quiet_NaN()));
    CHECK_FALSE(playback.begin(2, 2));
    CHECK_FALSE(playback.begin(1, 1));
    REQUIRE(playback.begin(1, 2));
    CHECK(playback.size() == 0);
    CHECK_FALSE(playback.sample(kStart));
    playback.clear();
    CHECK_FALSE(playback.latest());
    REQUIRE(playback.begin(2, 1));
}

TEST_CASE("projectile playback interpolates placement scale and effect phase on the shared clock",
          "[netplay][combat-playback]") {
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    auto first = state(10);
    ProjectileState shot;
    shot.instance = 1;
    shot.resource = 1;
    shot.direction = {0, 0, 20};
    shot.animation = {0, 0, 1, 0, 1};
    first.projectiles.push_back(shot);
    auto last = first;
    last.motion.tick = 14;
    auto& end = last.projectiles[0];
    end.placement = glm::scale(glm::rotate(Mat4{1}, kPi / 2, Vec3{0, 1, 0}), Vec3{3});
    end.placement[3].x = 8;
    end.age = 1;
    end.alpha = 0;
    end.radius = 3;
    end.textureFrame = 4;
    end.animation.frame = 4;
    receive(playback, first);
    receive(playback, last);
    const auto middle = playback.sample(12);
    REQUIRE(middle);
    REQUIRE(middle->projectiles.size() == 1);
    const auto& shown = middle->projectiles[0];
    CHECK(shown.placement[3].x == 4);
    CHECK(glm::length(Vec3{shown.placement[0]}) == Approx(2));
    CHECK(shown.placement[0].x == Approx(std::sqrt(2.0f)));
    CHECK(shown.placement[0].z == Approx(-std::sqrt(2.0f)));
    CHECK(shown.age == 0.5f);
    CHECK(shown.alpha == 0.5f);
    CHECK(shown.radius == 2);
    CHECK(shown.animation.frame == 2);
    CHECK(shown.textureFrame == 2);
    CHECK(CombatPacket::encode(*playback.sample(5000)) == CombatPacket::encode(last));
}

TEST_CASE("projectile cuts morphs births and despawns never blend across identities",
          "[netplay][combat-playback]") {
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    auto first = state(10);
    ProjectileState shot;
    shot.instance = 1;
    shot.resource = 1;
    shot.direction = {0, 0, 20};
    first.projectiles.push_back(shot);
    auto last = first;
    last.motion.tick = 14;
    last.projectiles[0].placement[3].x = 8;
    SECTION("reflected shot") {
        ++last.projectiles[0].continuity;
        last.projectiles[0].direction = {0, 0, -20};
    }
    SECTION("replacement") {
        ++last.projectiles[0].instance;
    }
    SECTION("different source pool") {
        last.projectiles[0].source = ProjectileSource::Enemy;
    }
    SECTION("morph to different tree") {
        ++last.projectiles[0].resource;
    }
    SECTION("lost teleport marker") {
        last.projectiles[0].placement[3].x = 100;
    }
    SECTION("despawn") {
        last.projectiles.clear();
    }
    SECTION("new birth") {
        auto born = shot;
        born.instance = 2;
        last.projectiles.push_back(born);
        last.projectiles[0].placement[3].x = 0;
    }
    receive(playback, first);
    receive(playback, last);
    const auto shown = playback.sample(13, 0.9f);
    REQUIRE(shown);
    REQUIRE(shown->projectiles.size() == 1);
    CHECK(shown->projectiles[0].key() == shot.key());
    CHECK(shown->projectiles[0].placement[3].x == 0);
    CHECK(shown->projectiles[0].direction.z == 20);
    CHECK(CombatPacket::encode(*playback.sample(14)) == CombatPacket::encode(last));
}

TEST_CASE("pickup riders share the platform clock while collection and visual cuts are discrete",
          "[netplay][combat-playback]") {
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    auto first = state(10);
    first.geometry = SceneGeometry{1, 1, 0, {{0}}};
    first.pickups.push_back(
        PickupState{.instance = 7, .resource = 1, .animation = {0, 0, 1, 0, 1}});
    auto last = first;
    last.motion.tick = 14;
    last.geometry->objects[0].local[3].y = -8;
    last.pickups[0].placement[3].y = -8;
    last.pickups[0].animation.frame = 4;
    last.pickups[0].textureFrame = 4;
    last.pickups[0].alpha = 0;
    bool continuous = false;
    SECTION("same platform attachment") {
        continuous = true;
    }
    SECTION("collected while the floor is moving") {
        last.pickups.clear();
    }
    SECTION("new loot has no earlier position") {
        ++last.pickups[0].instance;
    }
    SECTION("poisoned food or slag changes resource") {
        ++last.pickups[0].resource;
    }
    SECTION("released loot or instant platform cut") {
        ++last.pickups[0].continuity;
    }
    SECTION("large relocation without a marker") {
        last.pickups[0].placement[3].y = -100;
    }
    receive(playback, first);
    receive(playback, last);
    const auto middle = playback.sample(12);
    REQUIRE(middle);
    REQUIRE(middle->pickups.size() == 1);
    CHECK(middle->geometry->objects[0].local[3].y == -4);
    const auto& item = middle->pickups[0];
    CHECK(item.instance == 7);
    CHECK(item.placement[3].y == (continuous ? -4 : 0));
    CHECK(item.animation.frame == (continuous ? 2 : 0));
    CHECK(item.textureFrame == (continuous ? 2 : 0));
    CHECK(item.alpha == (continuous ? 0.5f : 1.0f));
    CHECK(CombatPacket::encode(*playback.sample(14)) == CombatPacket::encode(last));
    CHECK(CombatPacket::encode(*playback.sample(5000)) == CombatPacket::encode(last));
}

TreeInfo tree() {
    TreeInfo result;
    result.nodes.emplace_back();
    TrackInfo track;
    track.flags = TrackInfo::channelBit(3);
    track.frames = {0, 10};
    track.values = {0, 10};
    TreeSequenceInfo sequence;
    sequence.frames = 11;
    sequence.tracks.push_back(track);
    sequence.trackOfNode = {0};
    result.sequences.push_back(sequence);
    return result;
}
TEST_CASE("replica poses sample authored fractional keys and reject invalid asset cursors",
          "[netplay][replica-pose]") {
    const auto source = tree();
    ReplicaPose shown;
    CHECK_FALSE(shown.show({0, 0, 1, 0, 1}));
    shown.bind(&source);
    REQUIRE(shown.show({0, 0, 1, 2.5f, 1}));
    CHECK(shown.pose().matrices()[0][3].x == Approx(2.5f));
    REQUIRE(shown.show({0, 0, 2, 8, 0.5f}));
    CHECK(shown.pose().matrices()[0][3].x == Approx(5.25f));
    REQUIRE(shown.show({0, 0, 2, 8, 0.5f})); // repeated renders cannot advance the transition
    CHECK(shown.pose().matrices()[0][3].x == Approx(5.25f));
    for (const auto invalid : {CombatAnimation{0, 1, 1, 0, 1}, CombatAnimation{0, 0, 1, 11, 1},
                               CombatAnimation{0, 0, 1, -1, 1}, CombatAnimation{}}) {
        CHECK_FALSE(shown.show(invalid));
        CHECK_FALSE(shown.pose().posed());
    }
    REQUIRE(shown.show({0, 0, 20, 8, 0.25f})); // no invented earlier action after loss
    CHECK(shown.pose().matrices()[0][3].x == 8);
    REQUIRE(shown.rest());
    CHECK(shown.pose().matrices()[0][3].x == 0);
    shown.bind(nullptr);
    CHECK_FALSE(shown.rest());
}
TEST_CASE("zero-duration authored releases present frame zero without advancing gameplay",
          "[netplay][replica-pose]") {
    auto source = tree();
    source.sequences[0].frames = 0;
    source.sequences[0].tracks[0].values = {3, 10};
    ReplicaPose shown;
    shown.bind(&source);
    REQUIRE(ReplicaPose::accepts(&source, {11, 0, 1, 0, 1}));
    REQUIRE(shown.show({11, 0, 1, 0, 1}));
    CHECK(shown.pose().matrices()[0][3].x == 3);
    CHECK_FALSE(ReplicaPose::accepts(&source, {11, 0, 1, 0.01f, 1}));
    CHECK_FALSE(shown.show({11, 0, 1, 0.01f, 1}));
    CHECK_FALSE(ReplicaPose::accepts(nullptr, {11, 0, 1, 0, 1}));
}
} // namespace
