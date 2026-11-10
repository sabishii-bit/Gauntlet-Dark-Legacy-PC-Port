#include <algorithm>
#include <array>
#include <bit>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/audio/LegendsDreamcastSpeech.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "worlddata/mount.wad") ||
        !std::filesystem::is_regular_file(root.parent_path() / "disc/AUDIO/AUD_DATA.ROM")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    return root;
}

TEST_CASE("Dreamcast rune narration retains native count and plural phrase order",
          "[dreamcast-speech]") {
    CHECK(LegendsDreamcastSpeech::runestoneCue(0) == std::vector<u32>{0x10079});
    CHECK(LegendsDreamcastSpeech::runestoneCue(1) == std::vector<u32>{0x10068});
    for (u32 count = 2; count <= 12; ++count) {
        CHECK(LegendsDreamcastSpeech::runestoneCue(count) ==
              std::vector<u32>{0x1006aU + count, 0x10069});
    }
    CHECK(LegendsDreamcastSpeech::runestoneCue(13) == std::vector<u32>{0x10077});
}

TEST_CASE("Dreamcast entry narration resolves authored WAD prefixes and bound level names",
          "[dreamcast-speech][assets]") {
    const auto root = nativeRoot();
    LegendsDreamcastSpeech speech;
    REQUIRE(speech.open(root));
    const auto check = [&](std::string_view realm, std::string_view map,
                           std::span<const u32> expected) {
        const auto scope = formats::dreamcastLegendsFixtureScope(
            readFile(root / "worlddata" / (std::string{realm} + ".wad")), map);
        REQUIRE(scope);
        speech.setEntry(*scope);
        CHECK(speech.entryCue() == std::vector<u32>{expected.begin(), expected.end()});
    };
    check("mount", "levelb1", std::array<u32, 2>{0xf0007, 0xf0001});
    check("mount", "levelb6", std::array<u32, 2>{0xf0008, 0xf0006});
    check("battle", "levelh4", std::array<u32, 2>{0x150005, 0x150001});
    check("tower", "levelt1", {});
    check("secret", "levels1", {});
}

TEST_CASE("Dreamcast narration drains behind parked menus and survives owner transfer",
          "[dreamcast-speech][assets]") {
    const auto root = nativeRoot();
    AudioMixer mixer{48000};
    SoundPlayer output{mixer};
    LegendsDreamcastSpeech speech;
    LegendsDreamcastSpeech next;
    REQUIRE(speech.open(root));
    REQUIRE(next.open(root));
    const auto stage =
        formats::dreamcastLegendsFixtureScope(readFile(root / "worlddata/mount.wad"), "levelb1");
    REQUIRE(stage);
    speech.setEntry(*stage);
    const auto tower =
        formats::dreamcastLegendsFixtureScope(readFile(root / "worlddata/tower.wad"), "levelt1");
    REQUIRE(tower);
    next.setEntry(*tower);
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture observe{&output,
                                       [&](const auto& event) { events.push_back(event); }};
    REQUIRE(speech.enqueue(output, speech.entryCue()));
    REQUIRE(speech.enqueue(output, LegendsDreamcastSpeech::runestoneCue(2)));
    REQUIRE(events.size() == 2);
    CHECK(events[0].bank == "dreamcast/map_b");
    CHECK(events[0].call == 7);
    CHECK(events[1].bank == "dreamcast/announce");
    CHECK(events[1].call == 108);
    CHECK(events[1].after == events[0].handle);
    REQUIRE(next.continueFrom(output, speech));
    speech.stop(output);
    CHECK(next.entryCue().empty()); // The old level's name must not follow us.
    CHECK(next.playing(output));
    std::array<f32, 1600> samples{};
    f64 lateEnergy = 0;
    // No LegendsLevel update is required while results/shop retain the scene.
    for (usize tick = 0; tick < 900; ++tick) {
        output.update();
        mixer.mix(samples);
        if (tick > 240) {
            for (const auto sample : samples) {
                lateEnergy += static_cast<f64>(sample) * sample;
            }
        }
    }
    CHECK(lateEnergy > .001);
    CHECK_FALSE(next.playing(output));
    CHECK(output.voiceCount() == 0);
    CHECK(events.size() == 2);
    next.stop(output);
}

TEST_CASE("Dreamcast narration has no fallback when optional banks are absent",
          "[dreamcast-speech]") {
    AudioMixer mixer{48000};
    SoundPlayer output{mixer};
    LegendsDreamcastSpeech speech;
    formats::LegendsDreamcastFixtureScope entry;
    entry.entrySound = "S_ENTERING1B";
    entry.stageNameSound = "S_B1NAME";
    speech.setEntry(entry);
    CHECK(speech.entryCue().empty());
    CHECK_FALSE(speech.enqueue(output, LegendsDreamcastSpeech::runestoneCue(1)));
    CHECK_FALSE(speech.playing(output));
    CHECK(output.voiceCount() == 0);
}

TEST_CASE("Dreamcast runtime admits one native rune count for the combined party",
          "[dreamcast-speech][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/levelb1";
    const auto objects = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), objects);
    map.textures =
        formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 2);
    // A copy, not a reference: the level the adapter moves into adds to and edits its pickups.
    const auto pickup = [&]() -> LegendsPickup {
        const auto pickups = adapter->pickups();
        const auto rune = std::ranges::find_if(pickups, [](const auto& candidate) {
            return candidate.award && candidate.award->kind == LegendsRelic::Runestone;
        });
        REQUIRE(rune != pickups.end());
        return *rune;
    }();
    CharacterSave first;
    CharacterSave second;
    first.character = 0;
    second.character = 1;
    const s32 oldRune = pickup.award->index == 0 ? 1 : 0;
    second.progress()
        .legendsProgress(LegendsPlatform::Dreamcast)
        .add(LegendsRelic::Runestone, oldRune);
    PlayerActor a;
    PlayerActor b;
    a.spawn(0, first, nullptr, pickup.position, 0);
    b.spawn(1, second, nullptr, pickup.position + Vec3{100, 0, 100}, 0);
    std::array<PlayerActor*, 2> actors{&a, &b};
    LegendsLevel level{device, map, std::move(adapter), directory, test::dataDirectory()};
    AudioMixer mixer{48000};
    SoundPlayer output{mixer};
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture observe{&output,
                                       [&](const auto& event) { events.push_back(event); }};
    level.updateParty(actors, 1.0f / 60, false);
    const auto receipts = level.takePickups();
    const auto receipt = std::ranges::find(receipts, pickup.name, &ImportedPickup::name);
    REQUIRE(receipt != receipts.end());
    CHECK(receipt->taking.sound.empty());
    level.playSounds(output, Vec3{100000});
    const auto countPhrase = [](const auto& event) {
        return event.kind == SoundPlayer::Event::Kind::Play && event.bank == "dreamcast/announce" &&
               event.call == 108;
    };
    CHECK(std::ranges::count_if(events, countPhrase) == 1);
    for (usize tick = 0; tick < 5; ++tick) {
        level.updateParty(actors, 1.0f / 60, false);
        level.playSounds(output, Vec3{100000});
    }
    CHECK(std::ranges::count_if(events, countPhrase) == 1);
    level.stopSounds();
}

TEST_CASE("Dreamcast obelisk contact plays the complete native cue without distance muting",
          "[dreamcast-speech][dreamcast-obelisks][assets]") {
    const auto root = nativeRoot();
    const auto directory = root / "levels/levelb1";
    const auto objects = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), objects);
    map.textures =
        formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    // A copy, not a reference: the level the adapter moves into adds to and edits its pickups.
    const auto pickup = [&]() -> LegendsPickup {
        const auto pickups = adapter->pickups();
        const auto found = std::ranges::find_if(pickups, [](const auto& candidate) {
            return candidate.award && candidate.award->kind == LegendsRelic::Obelisk;
        });
        REQUIRE(found != pickups.end());
        return *found;
    }();
    const auto bodies = adapter->containerObstacles();
    const auto body = std::ranges::find(bodies, pickup.position, &Obstacle::centre);
    REQUIRE(body != bodies.end());
    CharacterSave save;
    save.character = 0;
    PlayerActor actor;
    actor.spawn(0, save, nullptr, pickup.position, 0);
    // Reproduce the contact point after the solid obelisk pushes the player out.
    const auto blockedPosition = body->pushOut(actor.position(), actor.radius());
    REQUIRE(blockedPosition != pickup.position);
    actor.spawn(0, save, nullptr, blockedPosition, 0);
    std::array<PlayerActor*, 1> party{&actor};
    LegendsLevel level{device, map, std::move(adapter), directory, test::dataDirectory()};
    AudioMixer mixer{48000};
    AudioMixer referenceMixer{48000};
    SoundPlayer output{mixer};
    SoundPlayer reference{referenceMixer};
    LegendsDreamcastSounds sounds;
    REQUIRE(sounds.open(root));
    REQUIRE(sounds.namedToken("S_OBELISKPKUP") == 0x5f);
    REQUIRE(sounds.play(reference, 0x5f) != kNoSound);
    level.updateParty(party, 1.0f / 60, false);
    CHECK(actor.save()
              .progress()
              .legendsProgress(LegendsPlatform::Dreamcast)
              .has(LegendsRelic::Obelisk, pickup.award->index));
    std::array<f32, 1600> actual{};
    std::array<f32, 1600> expected{};
    f64 energy = 0;
    for (usize tick = 0; tick < 300; ++tick) {
        level.playSounds(output, Vec3{100000});
        output.update();
        reference.update();
        mixer.mix(actual);
        referenceMixer.mix(expected);
        REQUIRE(actual == expected);
        for (const auto value : actual) {
            energy += static_cast<f64>(value) * value;
        }
    }
    CHECK(energy > .001);
    CHECK(output.voiceCount() == 0);
    level.stopSounds();
}
} // namespace
