#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    std::unique_ptr<LegendsLevel> level;

    Fixture() {
        const auto directory =
            test::dataDirectory().parent_path() / "assets/n64/extracted/levels/ice1";
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Extracted N64 Legends assets unavailable");
        }
        const auto models = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models);
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        level = std::make_unique<LegendsLevel>(device, map, std::move(adapter), directory,
                                               test::assetOrSkip("PLAYERS").parent_path());
    }

    LegendsContainerTarget barrel(u32 nativeIndex) const {
        const auto targets = level->adapter().containerTargets();
        const auto target = std::ranges::find_if(targets, [&](const auto& entry) {
            return entry.nativeTrap && entry.nativeIndex == nativeIndex;
        });
        REQUIRE(target != targets.end());
        REQUIRE(target->breakable);
        return *target;
    }

    bool present(s32 id) const {
        return std::ranges::any_of(level->targets(),
                                   [&](const auto& entry) { return entry.id == id; });
    }
};
} // namespace

TEST_CASE("Imported barrel events reach normal opponent callbacks and standing party members once",
          "[legends][level-effects][blasts][assets]") {
    Fixture fixture;
    bool poison = false;
    SECTION("red fire barrel") {}
    SECTION("green poison barrel") {
        poison = true;
    }
    const auto target = fixture.barrel(poison ? 31 : 21);
    CharacterSave save;
    save.progress().health = 500;
    PlayerActor actor;
    actor.spawn(2, save, nullptr, target.base, 0);
    std::array<PlayerActor*, 1> party{&actor};
    fixture.level->updateParty(party, 0, false);
    REQUIRE(fixture.level->strike(target.id, 1, 0, 2));
    CHECK_FALSE(fixture.level->strike(target.id, 1, 0, 2));
    usize opponentHits = 0;
    usize playerHits = 0;
    const auto apply = [&](const Vec3& position, f32 radius, f32 damage, u32 flags,
                           std::vector<s32>& reached) {
        CHECK(radius > 0);
        CHECK(damage > 0);
        CHECK((flags == 0x800U || flags == 0x421U));
        if (glm::distance(position, target.base + Vec3{0, 2, 0}) < .01f &&
            std::ranges::find(reached, 123) == reached.end()) {
            reached.push_back(123);
            ++opponentHits;
        }
    };
    fixture.level->affectArea(apply); // admission begins at radius zero
    CHECK(opponentHits == 0);
    CHECK(fixture.level->takeHazardHits().empty());
    for (usize tick = 0; tick < 180; ++tick) {
        fixture.level->update(1.0f / 60, false);
        fixture.level->affectArea(apply);
        for (const auto& hit : fixture.level->takeHazardHits()) {
            CHECK(hit.player == 2);
            CHECK(hit.damage > 0);
            CHECK((hit.impact.flags & damage::kLow) != 0);
            CHECK((hit.impact.flags & damage::kElementMask) == (hit.poison ? 0U : damage::kFire));
            if (!hit.poison) {
                CHECK(Damage::modify(hit.damage, hit.impact.flags, 0x100U, 0, false).amount == 0);
            }
            // Secondary chain explosions have their own victim admission.
            if (hit.poison == poison) {
                ++playerHits;
            }
        }
    }
    CHECK(opponentHits == 1);
    CHECK(playerHits == 1);
    CHECK(fixture.level->takeHazardHits().empty());
    CHECK_FALSE(fixture.present(target.id));
}

TEST_CASE("Native poison barrel chains detonate a nearby explosive through physical damage",
          "[legends][level-effects][blasts][assets]") {
    Fixture fixture;
    // IPR quarter-unit coordinates put green35 and red36 only 4.265 units
    // apart. Green31/red33 are 8.434 apart, outside this native blast.
    const auto green = fixture.barrel(35);
    const auto red = fixture.barrel(36);
    REQUIRE(glm::distance(green.base, red.base) < 4.5f);
    REQUIRE(fixture.level->strike(green.id, 1, 0, 0));
    fixture.level->affectArea({});
    REQUIRE(fixture.present(red.id));
    for (usize tick = 0; tick < 120 && fixture.present(red.id); ++tick) {
        fixture.level->update(1.0f / 60, false);
        fixture.level->affectArea({});
    }
    CHECK_FALSE(fixture.present(red.id));
    CHECK_FALSE(fixture.level->strike(green.id, 9999, 0, 0));
    CHECK_FALSE(fixture.level->strike(red.id, 9999, 0, 0));

    // Same threshold path as the live imported targets: poison's elemental
    // damage cannot turn its physical three into an ordinary barrel's six.
    LegendsContainerDefinition wood;
    wood.damageThreshold = 6;
    LegendsContainer ordinary{wood};
    LegendsFixtureEvent event;
    event.kind = LegendsFixtureEvent::Kind::Poison;
    event.position = green.base;
    event.radius = 4.5f;
    event.damage = 30;
    event.seconds = 1;
    event.noFalloff = true;
    const LegendsBlast poison{event};
    CHECK_FALSE(ordinary.strike(poison.physicalDamageAt(green.base)));
    CHECK(ordinary.targetable());
}

TEST_CASE("Imported barrel sounds obey effects volume and stop without replaying the event",
          "[legends][level-effects][audio][native-effects][assets]") {
    if constexpr (!GDL_LEGENDS_AUDIO) {
        SKIP("Native MusyX audio is disabled in this build");
    }
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    Fixture fixture;
    const auto red = fixture.barrel(21);
    output.setCategoryVolume(SoundCategory::Effects, 0);
    REQUIRE(fixture.level->strike(red.id, 1, 0, 0));
    fixture.level->playSounds(output, red.base);
    REQUIRE(output.voiceCount() == 1);
    fixture.level->playSounds(output, red.base);
    CHECK(output.voiceCount() == 1);
    std::array<f32, 1600> samples{};
    mixer.mix(samples);
    CHECK(std::ranges::all_of(samples, [](f32 sample) { return sample == 0; }));
    output.setCategoryVolume(SoundCategory::Effects, 1);
    f64 energy = 0;
    for (usize tick = 0; tick < 12; ++tick) {
        mixer.mix(samples);
        for (const f32 sample : samples) {
            energy += static_cast<f64>(sample) * sample;
        }
        output.update();
    }
    CHECK(energy > .0001);
    fixture.level->stopSounds();
    mixer.mix(samples); // allow the shared mixer's anti-click stop ramp
    mixer.mix(samples);
    output.update();
    CHECK(std::ranges::all_of(samples, [](f32 sample) { return sample == 0; }));
    CHECK(output.voiceCount() == 0);
    fixture.level->playSounds(output, red.base);
    CHECK(output.voiceCount() == 0);
    fixture.level->stopSounds();
}
