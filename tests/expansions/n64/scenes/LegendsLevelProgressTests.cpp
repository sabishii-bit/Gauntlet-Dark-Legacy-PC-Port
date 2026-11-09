#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Ambient.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct ProgressFixture {
    test::FakeRenderDevice device;
    std::unique_ptr<LegendsLevel> level;

    explicit ProgressFixture(std::string_view name) {
        const auto directory =
            test::dataDirectory().parent_path() / "assets/n64/extracted/levels" / name;
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Extracted N64 Legends assets unavailable");
        }
        const auto models = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                                 readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
        level = std::make_unique<LegendsLevel>(device, map, std::move(adapter), directory,
                                               test::assetOrSkip("PLAYERS").parent_path());
    }

    void admit(std::span<PartyMember> party, std::string_view destination) {
        const auto portals = level->adapter().portals();
        const auto found = std::ranges::find(portals, destination, &formats::LegendsN64Portal::map);
        REQUIRE(found != portals.end());
        level->initializeParty(party);
        std::array<PlayerActor, 4> actors;
        std::vector<PlayerActor*> visitors;
        for (const auto& member : party) {
            if (!member.fallen) {
                REQUIRE(member.player >= 0);
                REQUIRE(member.player < 4);
                auto& actor = actors[static_cast<usize>(member.player)];
                actor.spawn(member.player, member.save, nullptr, found->position, 0);
                visitors.push_back(&actor);
            }
        }
        for (usize tick = 0; tick < 600 && !level->destination(); ++tick) {
            level->capturePresentation();
            level->update(1.0f / 60, false);
            level->updateParty(visitors, 1.0f / 60, false);
        }
        REQUIRE(level->destination());
        CHECK(*level->destination() == destination);
        REQUIRE(level->departure());
        REQUIRE_FALSE(level->departure()->finished);
        // Mirror PlayScene::party(): completion applies to the up-to-date saves,
        // not the frozen entry copies or an unrelated class's inventory.
        for (auto& member : party) {
            if (!member.fallen) {
                member.save = actors[static_cast<usize>(member.player)].save();
            }
        }
    }
};

std::array<PartyMember, 3> progressionParty() {
    std::array<PartyMember, 3> party;
    for (usize index = 0; index < party.size(); ++index) {
        auto& member = party[index];
        member.player = static_cast<s32>(index);
        member.slot = index + 4;
        member.save.character = static_cast<s32>(index + 1);
        member.save.gold = 123 + static_cast<s32>(index);
        member.save.progress().health = 500;
        member.save.progress().relics.runes = 0x5;
        member.save.progress().levels.recordBeaten(1, 3);
        member.save.progress().legendsProgress(LegendsPlatform::Dreamcast).obelisks = 7;
        member.save.progress()
            .legendsProgress(LegendsPlatform::Dreamcast)
            .recordCompleted("levela1");
        member.save.classes[0].legendsProgress(LegendsPlatform::N64).recordCompleted("mount5");
    }
    party[2].fallen = true;
    return party;
}
} // namespace

TEST_CASE("Native Tower completion exits retain the party while their special transition runs",
          "[legends][level-progress][tower-special-portals][assets]") {
    const bool cathedral = GENERATE(false, true);
    ProgressFixture fixture{"tower2"};
    std::array<PartyMember, 2> party;
    party[0].player = 0;
    party[1].player = 2;
    for (auto& member : party) {
        member.save.progress().health = 9000;
        auto& progress = member.save.progress().legendsProgress(LegendsPlatform::N64);
        progress.runestones = 0x1fff;
        progress.shards = 0xf;
    }
    fixture.level->initializeParty(party);
    const Vec3 centre = cathedral ? Vec3{0, 1, -30} : Vec3{1.5f, 0, 14};
    std::array<PlayerActor, 2> actors;
    actors[0].spawn(0, party[0].save, nullptr, centre, .5f);
    actors[1].spawn(2, party[1].save, nullptr, centre + Vec3{10, 0, 0}, -.3f);
    std::array<PlayerActor*, 2> visitors{actors.data(), &actors[1]};
    for (s32 tick = 0; tick < 180; ++tick) {
        fixture.level->capturePresentation();
        fixture.level->update(1.0f / 60, false);
        fixture.level->updateParty(visitors, 1.0f / 60, false);
        REQUIRE_FALSE(fixture.level->destination());
    }
    actors[1].place(centre);
    for (s32 tick = 0; tick < 300 && !fixture.level->destination(); ++tick) {
        fixture.level->capturePresentation();
        fixture.level->update(1.0f / 60, false);
        fixture.level->updateParty(visitors, 1.0f / 60, false);
    }
    REQUIRE(fixture.level->destination());
    CHECK(*fixture.level->destination() == (cathedral ? "boss1" : "hell1"));
    for (s32 tick = 0; tick < 120; ++tick) {
        fixture.level->capturePresentation();
        fixture.level->update(1.0f / 60, false);
        const auto current = fixture.level->departure();
        REQUIRE(current);
        CHECK(current->yaw == 0);
        CHECK(current->scale == 1);
        CHECK(current->skin == nullptr);
        CHECK(current->keepVisible);
        CHECK_FALSE(current->transportSound);
        CHECK(current->whiteFade.has_value() == cathedral);
        CHECK(actors[0].position() == centre);
        CHECK(actors[1].position() == centre);
        if (cathedral) {
            CHECK(*current->whiteFade == (current->finished ? 1.0f : 0.0f));
            REQUIRE(current->fog);
            const auto expected = legendsCathedralFog(static_cast<f32>(tick + 1) / 60);
            CHECK(current->fog->bias == Catch::Approx(expected.bias));
            CHECK(current->fog->reciprocalScale == Catch::Approx(expected.reciprocalScale));
            const auto interpolated = fixture.level->departure(.5f);
            REQUIRE(interpolated);
            REQUIRE(interpolated->fog);
            CHECK(*interpolated->whiteFade == 0);
            CHECK(interpolated->fog->bias ==
                  Catch::Approx(legendsCathedralFog((static_cast<f32>(tick) + .5f) / 60).bias));
        }
    }
    REQUIRE(fixture.level->departure()->finished);
    REQUIRE(fixture.level->cancelDeparture());
    CHECK_FALSE(fixture.level->departure());
    CHECK_FALSE(fixture.level->destination());
    // Holding the same contact after a failed destination cannot start another
    // white fade immediately. Leaving and re-entering admits a fresh attempt.
    for (s32 tick = 0; tick < 300; ++tick) {
        fixture.level->updateParty(visitors, 1.0f / 60, false);
        REQUIRE_FALSE(fixture.level->destination());
    }
    for (auto& actor : actors) {
        actor.place(centre + Vec3{10, 0, 0});
    }
    fixture.level->updateParty(visitors, 1.0f / 60, false);
    for (auto& actor : actors) {
        actor.place(centre);
    }
    for (s32 tick = 0; tick < 300 && !fixture.level->destination(); ++tick) {
        fixture.level->updateParty(visitors, 1.0f / 60, false);
    }
    REQUIRE(fixture.level->destination());
    CHECK(fixture.level->departure()->yaw == 0);
    CHECK(fixture.level->departure()->scale == 1);
}

TEST_CASE("Completed native departures record only attending characters' departing platform stage",
          "[legends][level-progress][assets]") {
    ProgressFixture fixture{"mount1"};
    auto party = progressionParty();
    const auto entry = party;
    REQUIRE_FALSE(fixture.level->adapter().isTower());

    // A console close/jump has no admitted, completed departure to grant.
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        CHECK(party[index].save == entry[index].save);
    }

    fixture.admit(party, "tower2");
    const auto admitted = party;
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        CHECK(party[index].save == admitted[index].save);
    }

    fixture.level->update(kLegendsPortalDepartureSeconds, false);
    REQUIRE(fixture.level->departure()->finished);
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        auto expected = admitted[index].save;
        if (!party[index].fallen) {
            REQUIRE(expected.progress()
                        .legendsProgress(LegendsPlatform::N64)
                        .recordCompleted("mount1"));
        }
        CHECK(party[index].save == expected);
        CHECK(party[index].slot == admitted[index].slot);
        CHECK_FALSE(party[index]
                        .save.progress()
                        .legendsProgress(LegendsPlatform::N64)
                        .hasCompleted("tower2"));
        const auto saved = party[index].save.toJson();
        CHECK(CharacterSave::fromJson(saved).toJson() == saved);
    }

    const auto completed = party;
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        CHECK(party[index].save == completed[index].save);
    }
}

TEST_CASE("Leaving the native Tower never records Tower or destination completion",
          "[legends][level-progress][tower][assets]") {
    ProgressFixture fixture{"tower2"};
    auto party = progressionParty();
    REQUIRE(fixture.level->adapter().isTower());
    fixture.admit(party, "mount1");
    const auto before = party;
    fixture.level->update(kLegendsPortalDepartureSeconds, false);
    REQUIRE(fixture.level->departure()->finished);
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        CHECK(party[index].save == before[index].save);
        CHECK_FALSE(party[index]
                        .save.progress()
                        .legendsProgress(LegendsPlatform::N64)
                        .hasCompleted("tower2"));
        CHECK_FALSE(party[index]
                        .save.progress()
                        .legendsProgress(LegendsPlatform::N64)
                        .hasCompleted("mount1"));
    }
}

TEST_CASE("Native level pickups clear departure reminders and remain owned after reload",
          "[legends][level-progress][native-progress-flow][assets]") {
    // Dagger Peak's authored IPR contains both rune4 and obelisk2. Mountain
    // Valley contains only an obelisk, so cannot exercise the priority handoff.
    ProgressFixture fixture{"mount2"};
    auto party = progressionParty();
    party[0].save.progress().relics.runes = 0xffff;
    party[1].save.progress().legendsProgress(LegendsPlatform::Dreamcast).runestones = 0x1fff;
    fixture.level->initializeParty(party);
    REQUIRE(fixture.level->exitReminder() == 0x4057);
    const auto pickups = fixture.level->adapter().pickups();
    const auto obelisk = std::ranges::find_if(pickups, [](const auto& pickup) {
        return pickup.award && pickup.award->kind == LegendsRelic::Obelisk;
    });
    const auto rune = std::ranges::find_if(pickups, [](const auto& pickup) {
        return pickup.award && pickup.award->kind == LegendsRelic::Runestone;
    });
    REQUIRE(obelisk != pickups.end());
    REQUIRE(rune != pickups.end());
    REQUIRE(obelisk->award->index == 1);
    REQUIRE(rune->award->index == 3);
    std::array<PlayerActor, 2> actors;
    actors[0].spawn(0, party[0].save, nullptr, obelisk->position, 0);
    actors[1].spawn(1, party[1].save, nullptr, obelisk->position, 0);
    std::array<PlayerActor*, 2> players{actors.data(), &actors[1]};
    fixture.level->updateParty(players, 1.0f / 60, false);
    REQUIRE(fixture.level->exitReminder() == 0x4059U + rune->award->index);
    // A normal pickup update, not a direct progress mutation, clears the rune.
    actors[0].place(rune->position);
    fixture.level->updateParty(players, 1.0f / 60, false);
    CHECK_FALSE(fixture.level->exitReminder());
    for (usize i = 0; i < actors.size(); ++i) {
        party[i].save = CharacterSave::fromJson(actors[i].save().toJson());
        CHECK(party[i]
                  .save.progress()
                  .legendsProgress(LegendsPlatform::N64)
                  .has(LegendsRelic::Runestone, rune->award->index));
    }
    ProgressFixture loaded{"mount2"};
    loaded.level->initializeParty(party);
    CHECK_FALSE(loaded.level->exitReminder());
}

TEST_CASE("A failed native destination retains rewards without retrying until exit re-entry",
          "[legends][level-progress][native-progress-flow][assets]") {
    ProgressFixture fixture{"mount1"};
    auto party = progressionParty();
    party[0].save.progress().legendsProgress(LegendsPlatform::N64).obelisks = 1;
    fixture.admit(party, "tower2");
    fixture.level->update(kLegendsPortalDepartureSeconds, false);
    REQUIRE(fixture.level->departure()->finished);
    const auto before = party;
    REQUIRE(fixture.level->cancelDeparture());
    CHECK_FALSE(fixture.level->destination());
    CHECK_FALSE(fixture.level->departure());
    fixture.level->completeTravel(party);
    CHECK(party[0].save == before[0].save);
    CHECK_FALSE(
        party[0].save.progress().legendsProgress(LegendsPlatform::N64).hasCompleted("mount1"));
    const auto portals = fixture.level->adapter().portals();
    const auto exit =
        std::ranges::find(portals, std::string_view{"tower2"}, &formats::LegendsN64Portal::map);
    REQUIRE(exit != portals.end());
    std::array<PlayerActor, 2> actors;
    std::array<PlayerActor*, 2> players{actors.data(), &actors[1]};
    for (usize i = 0; i < actors.size(); ++i) {
        actors[i].spawn(static_cast<s32>(i), party[i].save, nullptr, exit->position, 0);
    }
    for (usize tick = 0; tick < 600; ++tick) {
        fixture.level->update(1.0f / 60, false);
        fixture.level->updateParty(players, 1.0f / 60, false);
        REQUIRE_FALSE(fixture.level->destination());
    }
    // The native contact volume, not elapsed time, releases the retry latch.
    const auto outside = exit->position + Vec3{100, 0, 0};
    for (auto& actor : actors) {
        const LegendsPortalVisitor visitor{static_cast<u8>(actor.player()), outside, true,
                                           actor.radius(), actor.height()};
        for (const auto& portal : portals) {
            REQUIRE_FALSE(touchesLegendsPortal(portal, visitor));
        }
        actor.place(outside);
    }
    fixture.level->updateParty(players, 1.0f / 60, false);
    for (auto& actor : actors) {
        actor.place(exit->position);
    }
    fixture.level->updateParty(players, 1.0f / 60, false);
    REQUIRE_FALSE(fixture.level->destination());
    for (usize tick = 0; tick < 600 && !fixture.level->destination(); ++tick) {
        fixture.level->update(1.0f / 60, false);
        fixture.level->updateParty(players, 1.0f / 60, false);
    }
    REQUIRE(fixture.level->destination() == "tower2");
    REQUIRE_FALSE(fixture.level->departure()->finished);
}

TEST_CASE("Native boss victory persists its completion and Battlefield obelisk independently",
          "[legends][level-progress][native-progress-flow][assets]") {
    ProgressFixture fixture{"boss2"};
    auto party = progressionParty();
    fixture.level->initializeParty(party);
    std::array<PlayerActor, 2> actors;
    for (usize i = 0; i < actors.size(); ++i) {
        actors[i].spawn(static_cast<s32>(i), party[i].save, nullptr, {0, 0, 0}, 0);
    }
    std::array<PlayerActor*, 2> players{actors.data(), &actors[1]};
    fixture.level->bossDefeated({0, 0, 0}, players);
    for (usize i = 0; i < actors.size(); ++i) {
        const auto loaded = CharacterSave::fromJson(actors[i].save().toJson());
        const auto& native = loaded.progress().legendsProgress(LegendsPlatform::N64);
        CHECK(native.hasCompleted("boss2"));
        CHECK(native.has(LegendsRelic::Obelisk, 7));
        CHECK(loaded.progress().relics == party[i].save.progress().relics);
        CHECK(loaded.progress().legendsProgress(LegendsPlatform::Dreamcast) ==
              party[i].save.progress().legendsProgress(LegendsPlatform::Dreamcast));
    }
    CHECK_FALSE(
        party[2].save.progress().legendsProgress(LegendsPlatform::N64).hasCompleted("boss2"));
    for (usize tick = 0; tick < 2400 && !fixture.level->destination(); ++tick) {
        fixture.level->update(1.0f / 60, false);
    }
    REQUIRE(fixture.level->destination());
    REQUIRE(fixture.level->cancelDeparture());
    for (usize tick = 0; tick < 2400; ++tick) {
        fixture.level->update(1.0f / 60, false);
        REQUIRE_FALSE(fixture.level->destination());
    }
    CHECK(actors[0].save().progress().legendsProgress(LegendsPlatform::N64).hasCompleted("boss2"));
}

TEST_CASE("Native departure narration survives scene teardown without repeating or clipping",
          "[legends][level-progress][native-progress-flow][native-speech][assets]") {
    const auto map = GENERATE(std::string_view{"mount1"}, std::string_view{"boss2"});
    CAPTURE(map);
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    ProgressFixture previous{map};
    ProgressFixture next{"tower2"};
    LegendsSpeech reference;
    if (!reference.openN64(previous.level->directory().parent_path().parent_path())) {
        SKIP("Native N64 narration extraction is not installed");
    }
    const u32 expectedToken = map == "boss2" ? 0x406bU : 0x4057U;
    const auto phrase = reference.resolve(expectedToken);
    REQUIRE(phrase);
    CHECK(phrase->clips ==
          (map == "boss2" ? std::vector<u16>{262, 246, 263, 247} : std::vector<u16>{246, 249}));
    auto party = progressionParty();
    if (map == "boss2") {
        previous.level->initializeParty(party);
        std::array<PlayerActor, 2> actors;
        for (usize i = 0; i < actors.size(); ++i) {
            actors[i].spawn(static_cast<s32>(i), party[i].save, nullptr, {}, 0);
        }
        std::array<PlayerActor*, 2> players{actors.data(), &actors[1]};
        previous.level->bossDefeated({}, players);
        for (usize tick = 0; tick < 2400 && !previous.level->destination(); ++tick) {
            previous.level->update(1.0f / 60, false);
        }
        REQUIRE(previous.level->destination());
        CHECK(*previous.level->destination() == "tower2");
    } else {
        previous.admit(party, "tower2");
    }
    REQUIRE(previous.level->exitReminder() == expectedToken);
    constexpr Vec3 kDistantListener{100000, 100000, 100000};
    previous.level->playSounds(output, kDistantListener);
    std::array<f32, 1600> samples{};
    for (usize tick = 0; tick < 240 && !previous.level->departure()->finished; ++tick) {
        previous.level->update(1.0f / 60, false);
        mixer.mix(samples);
        output.update();
    }
    REQUIRE(previous.level->departure()->finished);
    next.level->continueSpeechFrom(*previous.level);
    previous.level->stopSounds(); // Destruction of the old scene must not own this handle now.
    output.update();
    REQUIRE(output.voiceCount() == 1);
    f64 continuedEnergy = 0;
    for (usize tick = 0; tick < 900; ++tick) {
        next.level->update(1.0f / 60, false);
        next.level->playSounds(output, kDistantListener);
        mixer.mix(samples);
        output.update();
        if (tick > 30) {
            for (const f32 sample : samples) {
                continuedEnergy += static_cast<f64>(sample) * sample;
            }
        }
    }
    // Native groups4057/406b are 6.54/12.10 seconds. Speech must survive the
    // authored departure, and end naturally once every clip was read.
    CHECK(continuedEnergy > .001);
    CHECK(output.voiceCount() == 0);
    next.level->playSounds(output, kDistantListener);
    output.update();
    CHECK(output.voiceCount() == 0);
    next.level->stopSounds();
}

TEST_CASE("N64 queued exit warnings finish while the completed level is parked at results",
          "[legends][native-speech][native-progress-flow][assets]") {
    ProgressFixture scene{"mount1"};
    AudioMixer mixer{48000};
    SoundPlayer output{mixer};
    auto party = progressionParty();
    const auto nativeRoot = scene.level->directory().parent_path().parent_path();
    LegendsSpeech reference;
    if (!reference.openN64(nativeRoot)) {
        SKIP("Native N64 narration extraction is not installed");
    }
    const auto entryToken = reference.entryCue("mount1");
    REQUIRE(entryToken);
    const auto entryCue = reference.resolve(*entryToken);
    const auto exitCue = reference.resolve(0x4057);
    REQUIRE(entryCue);
    REQUIRE(exitCue);
    REQUIRE_FALSE(exitCue->clearQueue);
    REQUIRE(exitCue->lifetimeSeconds > 0);
    MortBank bank;
    REQUIRE(bank.open(nativeRoot / "audio/mort.mdir"));
    auto entrySource = bank.create(entryCue->clips);
    REQUIRE(entrySource);
    std::vector<f32> entryPcm;
    while (entrySource->read(entryPcm, 4096)) {
    }
    const f64 entrySeconds = static_cast<f64>(entryPcm.size()) / entrySource->desc().channels /
                             entrySource->desc().sampleRate;
    REQUIRE(entrySeconds > exitCue->lifetimeSeconds);
    // Start the native entry phrase before admitting the exit. The warning must
    // wait for it, exercising queue advancement rather than just an active voice.
    constexpr Vec3 kDistantListener{100000};
    const std::array<CameraSubject, 1> subjects{{{kDistantListener, kDistantListener, 1}}};
    scene.level->resetCamera(subjects);
    // Native ambience follows camera attention, not playSounds' fallback listener.
    // Keep it out of this narration-only assertion without muting the speech.
    const auto attention = scene.level->cameraAttention();
    REQUIRE(attention);
    for (const auto& emitter :
         formats::decodeLegendsN64Ambient(readFile(scene.level->directory() / "ipr.rom"))) {
        REQUIRE(glm::distance(*attention, emitter.position) > emitter.radius);
    }
    scene.level->playSounds(output, kDistantListener);
    REQUIRE(output.voiceCount() == 1);
    // The native warning expires after one second waiting. Consume the entry
    // until only half that lifetime remains; an unconsumed full entry correctly
    // expires the warning and would not test servicing a viable parked queue.
    const auto advanceTicks =
        static_cast<usize>(std::floor((entrySeconds - exitCue->lifetimeSeconds * .5) * 60));
    std::array<f32, 1600> samples{};
    for (usize tick = 0; tick < advanceTicks; ++tick) {
        output.update();
        mixer.mix(samples);
    }
    const f64 remainingEntrySeconds = entrySeconds - static_cast<f64>(advanceTicks) / 60;
    REQUIRE(remainingEntrySeconds > 0);
    REQUIRE(remainingEntrySeconds < exitCue->lifetimeSeconds);
    REQUIRE(output.voiceCount() == 1);
    scene.admit(party, "tower2");
    REQUIRE(scene.level->exitReminder() == 0x4057);
    scene.level->playSounds(output, kDistantListener);
    scene.level->pauseSounds();
    const auto clock = scene.level->departure();
    REQUIRE(clock);
    const auto entryEndTick = static_cast<usize>(std::ceil(remainingEntrySeconds * 60)) + 2;
    f64 lateEnergy = 0;
    for (usize tick = 0; tick < 1200; ++tick) {
        scene.level->servicePreservedSpeech(1.0f / 60);
        output.update();
        mixer.mix(samples);
        if (tick > entryEndTick)
            for (const auto sample : samples)
                lateEnergy += static_cast<f64>(sample) * sample;
    }
    CHECK(lateEnergy > .001);
    CHECK(output.voiceCount() == 0);
    // Results may pump only the speech queue, never advance the departed world.
    CHECK(scene.level->departure()->finished == clock->finished);
    scene.level->servicePreservedSpeech(1.0f / 60);
    CHECK(output.voiceCount() == 0);
    scene.level->stopSounds();
}
