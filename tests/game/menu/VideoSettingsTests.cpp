#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/menu/VideoSettings.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Video trials preserve the saved baseline through failed operations", "[video-trial]") {
    VideoSettings trial;
    const GameConfig saved;
    GameConfig draft = saved;
    draft.display.vsync = !saved.display.vsync;
    GameConfig active = saved;
    bool rejectPreview = false;
    bool rejectSave = false;
    s32 writes = 0;
    f64 now = 0;
    trial.begin(
        saved,
        [&](const auto& next) {
            if (rejectPreview) {
                return false;
            }
            active = next;
            return true;
        },
        [&](const auto&) {
            ++writes;
            return !rejectSave;
        },
        [&] { return now; });
    CHECK_FALSE(trial.confirm());
    SECTION("failed apply never starts a confirmation") {
        rejectPreview = true;
        CHECK_FALSE(trial.apply(draft));
        CHECK_FALSE(trial.pending());
        CHECK(active.display.vsync == saved.display.vsync);
    }
    SECTION("failed persistence rolls back without changing the baseline") {
        REQUIRE(trial.apply(draft));
        CHECK_FALSE(trial.apply(saved));
        rejectSave = true;
        CHECK_FALSE(trial.confirm());
        CHECK_FALSE(trial.pending());
        CHECK(active.display.vsync == saved.display.vsync);
        CHECK(trial.saved().display.vsync == saved.display.vsync);
        CHECK(writes == 1);
    }
    SECTION("expired confirmation cannot save and retries a failed rollback") {
        REQUIRE(trial.apply(draft));
        now = 15;
        rejectPreview = true;
        CHECK_FALSE(trial.confirm());
        CHECK(trial.pending());
        CHECK(writes == 0);
        CHECK_FALSE(trial.update());
        rejectPreview = false;
        CHECK(trial.update());
        CHECK_FALSE(trial.pending());
        CHECK(active.display.vsync == saved.display.vsync);
    }
    SECTION("successful save becomes the next rollback baseline") {
        REQUIRE(trial.apply(draft));
        REQUIRE(trial.confirm());
        CHECK(trial.saved().display.vsync == draft.display.vsync);
        REQUIRE(trial.apply(saved));
        REQUIRE(trial.revert());
        CHECK(active.display.vsync == draft.display.vsync);
        CHECK(writes == 1);
    }
}
} // namespace
