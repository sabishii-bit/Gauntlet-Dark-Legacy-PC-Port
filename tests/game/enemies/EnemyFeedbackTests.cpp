#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/enemies/EnemyFeedback.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("enemy sounds distinguish tier hit count and close versus far damage",
          "[game][enemies][enemy-feedback]") {
    EnemyFeedback feedback;
    feedback.kind = 13;
    CHECK(feedback.sound() == "S_ZOM1HITFAR");
    feedback.close = true;
    CHECK(feedback.sound() == "S_ZOM1HITCLOSE");
    feedback.tier = 2;
    CHECK(feedback.sound() == "S_ZOM2HIT1CLOSE");
    feedback.hitCount = 2;
    CHECK(feedback.sound() == "S_ZOM2HIT2CLOSE");
    feedback.killed = true;
    CHECK(feedback.sound() == "S_ZOM2DIECLOSE");
    feedback.close = false;
    CHECK(feedback.sound() == "S_ZOM2DIEFAR");
    feedback.tier = 1;
    CHECK(feedback.sound(true) == "S_ZOM2DIEFAR");
    feedback.kind = 27;
    CHECK(feedback.sound() == "S_GRM1DIEFAR");
    feedback.kind = -1;
    CHECK(feedback.sound().empty());
}

TEST_CASE("enemy impact and death skins retain elemental and nonflesh distinctions",
          "[game][enemies][enemy-feedback]") {
    EnemyFeedback feedback;
    feedback.kind = 13;
    feedback.halfHeight = 4;
    CHECK(feedback.effect() == "BLOODFX1");
    CHECK(feedback.effectScale() == Approx(2));
    CHECK(feedback.deathSkin() == "DEATHBLOOD");
    CHECK(feedback.deathSkinFrames() == 10);
    feedback.killed = true;
    CHECK(feedback.effect() == "BLOODFX2");
    feedback.flags = 1;
    CHECK(feedback.effect() == "FIREDIE");
    CHECK(feedback.deathSkin() == "DEATHFIRE");
    feedback.flags = 2;
    CHECK(feedback.effect() == "ELECDIE");
    CHECK(feedback.deathSkin() == "DEATHELEC");
    feedback.flags = 3;
    CHECK(feedback.effect() == "LIGHTDIE");
    feedback.flags = 4;
    CHECK(feedback.effect() == "ACIDDIE");
    feedback.flags = 0;
    feedback.kind = 5;
    CHECK(feedback.effect() == "HITDIE");
    CHECK(feedback.effectScale() == Approx(1));
    CHECK(feedback.deathSkin() == "DEATHALT");
    CHECK(feedback.deathSkinFrames() == 15);
    feedback.kind = 11;
    CHECK(feedback.deathSkinFrames() == 10);
    CHECK(feedback.effect() == "TREEDIE");
    feedback.killed = false;
    CHECK(feedback.effect() == "TREEHIT");
    feedback.kind = 21;
    feedback.halfHeight = 1;
    CHECK(feedback.effect() == "TREEHIT");
    CHECK(feedback.deathSkin().empty());
    CHECK(feedback.deathSkinFrames() == 0);
    feedback.flags = 0x1000000;
    CHECK(feedback.effect().empty());
    CHECK_FALSE(feedback.sound().empty());
    feedback.flags = 15;
    CHECK(feedback.effect().empty());
    CHECK(feedback.deathSkin().empty());
}
} // namespace
