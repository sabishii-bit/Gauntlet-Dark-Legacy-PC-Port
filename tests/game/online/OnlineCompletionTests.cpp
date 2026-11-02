#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/codec/MoviePlayback.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Party.h"
#include "game/config/GameConfig.h"
#include "game/context/GameContext.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/level/TowerAccess.h"
#include "game/online/OnlineCompletion.h"
#include "game/shop/ShopSession.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("online completion selects only defeated final boss movies and final results",
          "[online-completion]") {
    LevelRef source;
    for (s32 kind = -1; kind <= 44; ++kind) {
        for (const bool defeated : {false, true}) {
            source.realmId = kind == 44 ? 8 : 6;
            const auto route = OnlineCompletion::route(source, kind, defeated);
            CAPTURE(kind, defeated);
            const bool ending = defeated && (kind == 43 || kind == 44);
            const std::string_view bossMovie = kind == 44 ? "victory" : "garm";
            CHECK(route.movie == (ending ? bossMovie : ""));
            CHECK(route.visit ==
                  (defeated && kind == 44 ? ShopVisit::Completion : ShopVisit::Level));
            CHECK(route.arrivalWorld == static_cast<u32>(source.realmId));
            CHECK_FALSE(route.skippable);
        }
    }
    source.realmId = -1;
    CHECK(OnlineCompletion::route(source, -1, false).arrivalWorld == 0);
}

TEST_CASE("online completion fails closed for absent or corrupt ending media",
          "[online-completion]") {
    test::FakeRenderDevice device;
    GameContext context;
    context.unpackedRoot = test::scratchDirectory("online-completion-missing");
    LevelRef source;
    source.realmId = 8;
    OnlineCompletion completion;
    REQUIRE(completion.phase() == OnlineCompletion::Phase::Closed);
    completion.open(device, context, source, 44, false);
    CHECK(completion.phase() == OnlineCompletion::Phase::Ready);
    CHECK_FALSE(completion.visible());
    CHECK(completion.moviePath().empty());
    completion.open(device, context, source, 44, true);
    CHECK(completion.phase() == OnlineCompletion::Phase::Failed);
    CHECK_FALSE(completion.visible());
    completion.update(context, true);
    CHECK(completion.phase() == OnlineCompletion::Phase::Failed);
    std::filesystem::create_directory(context.unpackedRoot / "VQMOVIES");
    const auto corrupt = context.unpackedRoot / "VQMOVIES/victory.avi";
    {
        std::ofstream file(corrupt, std::ios::binary);
        file << "not an AVI";
    }
    completion.open(device, context, source, 44, true);
    CHECK(completion.phase() == OnlineCompletion::Phase::Failed);
    CHECK(completion.moviePath() == corrupt);
    CHECK_FALSE(completion.visible());
    completion.close();
    CHECK(completion.phase() == OnlineCompletion::Phase::Closed);
    CHECK(completion.moviePath().empty());
    CHECK(completion.selection().movie.empty());
}

TEST_CASE("ending returns use the native source-realm Tower marker and unlock fallback",
          "[online-completion][assets]") {
    const auto root = test::assetOrSkip("VQMOVIES/victory.avi").parent_path().parent_path();
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE(tower.load(device, root, LevelRef::tower()));
    CharacterSave save;
    save.progress().relics.shards = TowerAccess::kBattlefieldShards;
    save.progress().relics.runes = TowerAccess::kGarmRunes;
    const std::array saves{save};
    const TowerAccess unlocked(saves);
    const TowerAccess locked;
    for (const s32 kind : {43, 44}) {
        const auto source = levels.byName(kind == 44 ? "H4" : "F2");
        REQUIRE(source);
        CHECK(source->realmId == (kind == 44 ? 8 : 6));
        const auto route = OnlineCompletion::route(*source, kind, true);
        const u32 expectedMarker = kind == 44 ? 11 : 10;
        REQUIRE(tower.startPoint(expectedMarker));
        CHECK(tower.arrivalPoint(route.arrivalWorld, &unlocked) ==
              tower.startPoint(expectedMarker));
        CHECK(tower.arrivalPoint(route.arrivalWorld, &locked) == tower.startPoint(0));
    }
}

TEST_CASE("native online endings ignore unanimous skips and retain their last frame at the barrier",
          "[online-completion][assets][movie]") {
    for (const s32 kind : {43, 44}) {
        const std::string name = kind == 44 ? "victory" : "garm";
        const auto file = test::assetOrSkip("VQMOVIES/" + name + ".avi");
        MoviePlayback metadata;
        REQUIRE(metadata.open(file));
        const auto duration = metadata.info().frameCount / metadata.info().framesPerSecond;
        CHECK(duration < 26);
        const auto expectedTicks = static_cast<s32>(std::ceil(duration * 60));
        metadata.close();
        test::FakeRenderDevice device;
        GameConfig config;
        config.game.autoSave = true;
        config.save.directory =
            (test::scratchDirectory("online-completion-no-save") / "characters").string();
        GameContext context;
        context.unpackedRoot = file.parent_path().parent_path();
        context.config = &config;
        s32 saves = 0;
        context.saveLocalParty = [&](std::span<PartyMember>) { ++saves; };
        LevelRef source;
        source.realmId = kind == 44 ? 8 : 6;
        OnlineCompletion completion;
        completion.open(device, context, source, kind, true);
        REQUIRE(completion.phase() == OnlineCompletion::Phase::Movie);
        CHECK(completion.moviePath() == file);
        CHECK(completion.selection().movie == name);
        CHECK_FALSE(completion.selection().skippable);
        // A peer whose first frame shows late, but within the wait, still sees the whole movie.
        for (u32 tick = 1; tick < OnlineCompletion::kUnshownTicks; ++tick) {
            completion.update(context, true);
        }
        REQUIRE(completion.phase() == OnlineCompletion::Phase::Movie);
        const Mat4 projection = makeScreenProjection(640, 448);
        completion.render(device, projection, 640, 448);
        REQUIRE(device.draws.size() == 1);
        CHECK(test::minCorner(device.draws.front()) == Vec2(0, 0));
        CHECK(test::maxCorner(device.draws.front()) == Vec2(640, 448));
        device.draws.clear();
        s32 elapsed = 0;
        while (completion.phase() == OnlineCompletion::Phase::Movie &&
               elapsed < expectedTicks + 60) {
            completion.update(context, true); // even every seat voting cannot skip state 2
            ++elapsed;
        }
        CAPTURE(name, duration, elapsed);
        REQUIRE(completion.phase() == OnlineCompletion::Phase::Ready);
        // The first update only starts the clock, and sixtieths summed can fall a hair short
        // of the last frame: one or two updates past the movie's length.
        CHECK(elapsed >= expectedTicks - 1);
        CHECK(elapsed <= expectedTicks + 2);
        REQUIRE(completion.visible());
        completion.render(device, projection, 640, 448);
        REQUIRE(device.draws.size() == 1);
        const auto* retained = device.draws.front().texture;
        const auto uploads = device.textureUpdates;
        const auto textures = device.texturesCreated;
        for (s32 tick = 0; tick < 180; ++tick) {
            completion.update(context, true);
        }
        device.draws.clear();
        completion.render(device, projection, 640, 448);
        REQUIRE(device.draws.size() == 1);
        CHECK(device.draws.front().texture == retained);
        CHECK(device.textureUpdates == uploads);
        CHECK(device.texturesCreated == textures);
        CHECK(saves == 0);
        CHECK_FALSE(std::filesystem::exists(config.save.directory));
        completion.close();
        CHECK_FALSE(completion.visible());
        // One that never shows (a minimised window) plays unseen after the wait, ending as
        // much later and not holding the others any longer.
        OnlineCompletion unseen;
        unseen.open(device, context, source, kind, true);
        s32 unseenTicks = 0;
        while (unseen.phase() == OnlineCompletion::Phase::Movie &&
               unseenTicks < expectedTicks + 200) {
            unseen.update(context, true);
            ++unseenTicks;
        }
        CHECK(unseen.phase() == OnlineCompletion::Phase::Ready);
        CHECK(unseenTicks == elapsed + static_cast<s32>(OnlineCompletion::kUnshownTicks) - 1);
    }
}

TEST_CASE("online ending volume is local and live while movie geometry ignores gameplay FoV",
          "[online-completion][movie-volume][assets]") {
    const auto file = test::assetOrSkip("VQMOVIES/garm.avi");
    test::FakeRenderDevice fullDevice;
    test::FakeRenderDevice quietDevice;
    AudioMixer fullMixer{48000};
    AudioMixer quietMixer{48000};
    GameConfig fullConfig;
    fullConfig.audio.masterVolume = 0.5f;
    fullConfig.audio.movieVolume = 0.5f;
    fullConfig.audio.musicVolume = 0;
    fullConfig.audio.effectsVolume = 0;
    fullConfig.camera.horizontalFovDegrees = 45;
    GameConfig quietConfig = fullConfig;
    quietConfig.audio.movieVolume = 0.25f;
    quietConfig.audio.musicVolume = 1;
    quietConfig.audio.effectsVolume = 1;
    quietConfig.camera.horizontalFovDegrees = 110;
    const AssetLocator assets(file.parent_path().parent_path());
    GameContext fullContext;
    fullContext.assets = &assets; // Explicit native locator takes precedence over fallback.
    fullContext.config = &fullConfig;
    fullContext.movieMixer = &fullMixer;
    GameContext quietContext = fullContext;
    quietContext.config = &quietConfig;
    quietContext.movieMixer = &quietMixer;
    OnlineCompletion full;
    OnlineCompletion quiet;
    LevelRef source;
    source.realmId = 6;
    full.open(fullDevice, fullContext, source, 43, true);
    quiet.open(quietDevice, quietContext, source, 43, true);
    REQUIRE(full.phase() == OnlineCompletion::Phase::Movie);
    REQUIRE(quiet.phase() == OnlineCompletion::Phase::Movie);
    const Mat4 projection = makeScreenProjection(1280, 896);
    full.render(fullDevice, projection, 1280, 896);
    quiet.render(quietDevice, projection, 1280, 896);
    REQUIRE(fullDevice.draws.size() == 1);
    REQUIRE(quietDevice.draws.size() == 1);
    CHECK(test::minCorner(fullDevice.draws.front()) == test::minCorner(quietDevice.draws.front()));
    CHECK(test::maxCorner(fullDevice.draws.front()) == test::maxCorner(quietDevice.draws.front()));
    CHECK(fullDevice.draws.front().transform == quietDevice.draws.front().transform);
    std::array<f32, 1600> fullAudio{};
    std::array<f32, 1600> quietAudio{};
    f32 peak = 0;
    for (s32 tick = 0; tick < 240; ++tick) {
        if (tick == 80) {
            quietConfig.audio.movieVolume = 0;
        } else if (tick == 160) {
            quietConfig.audio.movieVolume = 0.25f;
        }
        full.update(fullContext, true);
        quiet.update(quietContext, true);
        fullMixer.mix(fullAudio);
        quietMixer.mix(quietAudio);
        const f32 ratio = tick >= 80 && tick < 160 ? 0 : 0.5f;
        f32 error = 0;
        for (usize sample = 512; sample < fullAudio.size(); ++sample) {
            peak = std::max(peak, std::abs(fullAudio[sample]));
            error = std::max(error, std::abs(quietAudio[sample] - fullAudio[sample] * ratio));
        }
        CAPTURE(tick);
        CHECK(error < 0.00001f);
    }
    CHECK(peak > 0.001f);
    CHECK(full.phase() == OnlineCompletion::Phase::Movie);
    CHECK(quiet.phase() == OnlineCompletion::Phase::Movie);
    quiet.close();
    quietMixer.mix(quietAudio);
    // AudioStream::stop preserves its five-millisecond anti-click fade.
    CHECK(std::ranges::all_of(std::span(quietAudio).subspan(512),
                              [](f32 sample) { return sample == 0; }));
    quietMixer.mix(quietAudio);
    CHECK(std::ranges::all_of(quietAudio, [](f32 sample) { return sample == 0; }));
}
} // namespace
