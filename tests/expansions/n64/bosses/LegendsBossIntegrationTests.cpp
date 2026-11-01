#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/audio/LegendsSoundTable.h"
#include "expansions/n64/audio/LegendsSounds.h"
#include "expansions/n64/formats/LegendsN64Critter.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/level/LegendsBossEruption.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/critters/CombatantAssets.h"
#include "game/enemies/LevelOpponents.h"

namespace {
using namespace gdl;
using namespace gdl::game;

formats::LegendsMap nativeMap(const std::filesystem::path& directory) {
    const auto models = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                             readFile(directory / "ipr.rom"));
    const auto dynamicPath = directory / "texdyn.rom";
    const auto dynamic =
        std::filesystem::is_regular_file(dynamicPath) ? readFile(dynamicPath) : std::vector<u8>{};
    for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    return map;
}

LevelOpponents::Events noPlayerDamage() {
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.destroyedGenerator = [](s32) {};
    events.shake = [] {};
    events.help = [](s32, usize) { return false; };
    return events;
}
} // namespace

TEST_CASE("Native bosses preserve the exact shipped silent-cue omissions",
          "[legends][bosses][native-boss-audio][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin"))
        SKIP("Native N64 extraction is not installed");
    const auto table = decodeLegendsSoundTable(readFile(root / "_program/game.bin"));
    struct Bank {
        std::string_view name;
        usize routed;
        usize silent;
    };
    constexpr std::array banks{Bank{"dragon", 12, 2}, Bank{"yeti", 12, 3}, Bank{"chi", 16, 13},
                               Bank{"slime", 13, 5},  Bank{"sko1", 16, 3}, Bank{"sko2", 9, 7}};
    std::set<std::string> absent;
    for (const auto& bank : banks) {
        CAPTURE(bank.name);
        const auto decoded =
            formats::decodeLegendsN64Critter(readFile(root / bank.name / "moves.wad"));
        usize routed = 0;
        usize silent = 0;
        for (const auto& sound : decoded.sounds) {
            if (sound.levelFormat.empty())
                continue;
            if (legendsSoundToken(table, sound.levelFormat)) {
                ++routed;
            } else {
                ++silent;
                absent.insert(sound.levelFormat);
            }
        }
        CHECK(routed == bank.routed);
        CHECK(silent == bank.silent);
    }
    // e0880320 stores -1 for these names, and e0881b84 skips their sound while
    // retaining visuals/links. They are native authoring leftovers, not aliases
    // for GameCube samples or a permission to ignore future missing names.
    const std::set<std::string> expected{
        "S_CHIMBITE",    "S_CHIMHITCLOSE",  "S_CHIMHITFAR",    "S_CHIMJUMP1",
        "S_CHIMJUMP2",   "S_CHIMPHIT1",     "S_CHIMPHIT2",     "S_CHIMPHIT3",
        "S_DRGHITCLOSE", "S_DRGHITFAR",     "S_PLGATTCK2",     "S_PLGATTCK5",
        "S_PLGHITCLOSE", "S_PLGHITFAR",     "S_SKORN1HITCLOS", "S_SKORN1HITFAR",
        "S_SKORN1ROAR",  "S_SKORN2ATTCK10", "S_SKORN2ATTCK13", "S_SKORN2ATTCK9",
        "S_SKORN2GEN",   "S_SKORN2HITCLOS", "S_SKORN2HITFAR",  "S_SKORN2ROAR",
        "S_YETIGRAB",    "S_YETIHITFAR",    "S_YETISHATTER"};
    CHECK(absent == expected);
}

TEST_CASE("All native Legends boss arenas open through the common opponent owner",
          "[legends][bosses][legends-boss-integration][assets]") {
    const auto nativeRoot = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(nativeRoot / "_program/Boss.bin")) {
        SKIP("native N64 boss extraction is not installed");
    }
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(nativeRoot / "_program/game.bin");
    const auto soundTable = decodeLegendsSoundTable(program);
    const auto overlay = readFile(nativeRoot / "_program/Boss.bin");
    LegendsSounds nativeSounds;
    if constexpr (GDL_LEGENDS_AUDIO) {
        REQUIRE(nativeSounds.openN64(nativeRoot));
    }
    for (const char* const name : {"mount6", "ice5", "town5", "castle5", "boss2", "hell2"}) {
        DYNAMIC_SECTION(name) {
            const auto encounter = legendsN64Boss(name, program, overlay);
            REQUIRE(encounter);
            const auto directory = nativeRoot / "levels" / name;
            auto map = nativeMap(directory);
            test::FakeRenderDevice device;
            auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
            auto imported =
                std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, root);
            REQUIRE(imported->importedBoss());
            CHECK(imported->importedBoss()->kind == encounter->combat.kind);
            CHECK(imported->arenaTargets().size() == encounter->combat.arenaRocks.size());
            CHECK(imported->arenaAnchors().size() == encounter->combat.arenaRocks.size());

            CharacterSave save;
            save.progress().legendsProgress(LegendsPlatform::N64).runestones = 0x1fff;
            std::array<PartyMember, 1> party;
            party[0].player = 0;
            party[0].save = save;
            imported->initializeParty(party);
            LegendsTraversal traversal;
            traversal.open(map, 0, save);
            LevelWorld world;
            LevelRef ref;
            ref.realmId = -1;
            ref.name = "n64:" + std::string(name);
            REQUIRE(world.loadImported(
                device, root, ref, WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                traversal.collision(), imported));
            CHECK(world.ref().name == ref.name);
            CHECK(world.level() == nullptr); // donor's campaign/tuning must never become this map
            ItemArchive weapons;
            REQUIRE(weapons.load(root / "WEAPONS"));
            EffectTrees effects;
            LevelSoundscape audio;
            audio.open(root, nullptr, nullptr); // Only common GDL cues belong to this soundscape.
            SoundSet donor;
            REQUIRE(donor.load(root / "AUDIO" / encounter->combat.soundBank));
            std::vector<std::string> donorOnlyCues;
            for (u32 index = 0; index < donor.size(); ++index) {
                const auto& cueName = donor.entry(index).name;
                if (!cueName.empty() && !audio.hasNamed(cueName)) {
                    donorOnlyCues.push_back(cueName);
                }
            }
            REQUIRE_FALSE(donorOnlyCues.empty());
            std::array<PlayerRuntime, 1> players;
            players[0].actor.spawn(0, save, nullptr, encounter->combat.position + Vec3{0, 0, 10},
                                   0);
            LevelOpponents opponents;
            opponents.open({device, world, weapons, effects, audio, root}, players);
            // Native boss opening must not install the counterpart's sound bank,
            // even when its asset names happen to overlap native S_* identifiers.
            for (const auto& cue : donorOnlyCues) {
                INFO(cue);
                CHECK_FALSE(audio.hasNamed(cue));
            }
            REQUIRE(opponents.bosses().present());
            CHECK(opponents.bosses().view().kind == encounter->combat.kind);
            CHECK(opponents.bosses().view().alive);
            CHECK_FALSE(opponents.bosses().view().awake);
            REQUIRE(opponents.bosses().position());
            CHECK(opponents.bosses().position()->x == encounter->combat.position.x);
            CHECK(opponents.bosses().position()->z == encounter->combat.position.z);
            usize cues = 0;
            for (const auto* assets : opponents.bosses().resources()) {
                REQUIRE(assets);
                // Native DESC names identify art; these three move banks have
                // separate loader names (game.bin e006b110 / Boss.bin setup).
                const std::string_view bank = encounter->combat.kind == 35   ? "chi"
                                              : encounter->combat.kind == 42 ? "sko1"
                                              : encounter->combat.kind == 43
                                                  ? "sko2"
                                                  : assets->data.folder();
                const auto source =
                    formats::decodeLegendsN64Critter(readFile(nativeRoot / bank / "moves.wad"));
                REQUIRE(source.sounds.size() == assets->data.sounds().size());
                for (usize index = 0; index < source.sounds.size(); ++index) {
                    const auto& effect = assets->data.sounds()[index];
                    const auto& authored = source.sounds[index];
                    std::string repaired =
                        encounter->combat.kind == 42 && authored.levelFormat == "S_SKORN1ROAR"
                            ? "S_SKORN1PROAR"
                            : authored.levelFormat;
                    if (encounter->combat.kind == 43) {
                        struct Alias {
                            std::string_view authored;
                            std::string_view replacement;
                            u32 row;
                            u16 effect;
                        };
                        constexpr std::array aliases{
                            Alias{"S_SKORN2ATTCK9", "S_SKORN9ATTCK9", 50, 134},
                            Alias{"S_SKORN2ATTCK10", "S_SKORN9ATTCK10", 51, 125},
                            Alias{"S_SKORN2ROAR", "S_SKORN2PROAR", 55, 132}};
                        for (const auto& alias : aliases) {
                            if (authored.levelFormat != alias.authored)
                                continue;
                            repaired = alias.replacement;
                            // These explicit adapter repairs select named-only
                            // native rows, not GameCube cues or numeric aliases.
                            CHECK_FALSE(legendsSoundToken(soundTable, alias.authored));
                            CHECK(legendsSoundToken(soundTable, repaired) ==
                                  (0x10000U | alias.row));
                            REQUIRE(alias.row < soundTable.size());
                            CHECK(soundTable[alias.row].token == 0xffff);
                            CHECK(soundTable[alias.row].replacement == alias.effect);
                            if constexpr (GDL_LEGENDS_AUDIO) {
                                const auto resolved = nativeSounds.resolve(0x10000U | alias.row);
                                REQUIRE(resolved);
                                CHECK(resolved->effect == alias.effect);
                            }
                        }
                    }
                    const auto routed = legendsSoundToken(soundTable, repaired);
                    CHECK(effect.soundFormat == (routed ? repaired : ""));
                    CHECK(effect.tree == authored.name);
                    // Native Plague attaches bit1 cues to the animation's
                    // container, above its translated TOP child. The common
                    // root-parent bit preserves that exact parent identity.
                    const u32 expectedFlags =
                        encounter->combat.kind == 38 && (authored.flags & 1U) != 0
                            ? (authored.flags & ~1U) | 0x800U
                            : authored.flags;
                    CHECK(effect.flags == expectedFlags);
                    CHECK(effect.link == authored.link);
                    const auto cue = effect.soundFor(encounter->combat.counterpartStage.front());
                    if (cue.empty()) {
                        continue;
                    }
                    INFO(cue);
                    CHECK(imported->playOpponentSound(cue, encounter->combat.position));
                    if constexpr (GDL_LEGENDS_AUDIO) {
                        const auto token = nativeSounds.namedToken(cue);
                        REQUIRE(token);
                        CHECK(nativeSounds.resolve(*token));
                    }
                    ++cues;
                }
            }
            CHECK(cues > 0);
            EnemyHit hit;
            hit.player = 0;
            hit.damage = 100;
            const f32 initial = opponents.bosses().view().health;
            opponents.bosses().hurt(hit);
            CHECK(opponents.bosses().view().health == initial);
            auto events = noPlayerDamage();
            events.arenaAnchors = [&] { return imported->arenaAnchors(); };
            events.arenaTargets = [&] { return imported->arenaTargets(); };
            events.activateArena = [&](const CombatArenaActivation& action) {
                imported->activateArena(action);
            };
            std::vector<usize> dormantFragments;
            for (usize index = 0; index < map.objects.size(); ++index) {
                if ((map.objects[index].flags & 0x200U) != 0) {
                    dormantFragments.push_back(index);
                    CHECK(imported->adapter().worldObjectVisible(index) ==
                          (encounter->combat.kind == 42));
                }
            }
            const bool skorneEntrance = encounter->combat.kind == 42;
            if (skorneEntrance) {
                REQUIRE(dormantFragments.size() == 43);
            }
            constexpr f32 kStep = 1.0f / 60;
            constexpr s32 kObservationFrames = 360;
            bool witnessedWake = false;
            s32 wakeFrame = -1;
            for (s32 frame = 0; frame < kObservationFrames; ++frame) {
                REQUIRE(opponents.bosses().position());
                players[0].actor.place(*opponents.bosses().position() + Vec3{0, 0, 10});
                imported->capturePresentation();
                imported->update(kStep, false);
                const bool wasAwake = opponents.bosses().view().awake;
                opponents.update(1, kStep, players, {}, events);
                if (!wasAwake) {
                    const bool waking = opponents.bosses().view().awake;
                    witnessedWake |= waking;
                    if (waking) {
                        wakeFrame = frame;
                    }
                    for (const usize object : dormantFragments) {
                        CHECK(imported->adapter().worldObjectVisible(object) == skorneEntrance);
                    }
                    if (waking && skorneEntrance) {
                        // The actual LevelOpponents wake edge, not a direct test
                        // call or a loading clock, releases the dormant geometry.
                        for (const usize object : dormantFragments) {
                            CHECK(imported->adapter().worldObjectTransform(object) == Mat4{1});
                        }
                    }
                }
            }
            CHECK(witnessedWake);
            REQUIRE(opponents.bosses().view().awake);
            REQUIRE(wakeFrame >= 0);
            // The native family arms at four seconds, independently of the
            // debris clock. Allow one tick for the float wake accumulator.
            const f64 wakeSeconds = static_cast<f64>(wakeFrame + 1) * kStep;
            CHECK(wakeSeconds >= 4);
            CHECK(wakeSeconds <= 4 + 2 * static_cast<f64>(kStep));
            if (skorneEntrance) {
                // updateArena runs before the actual wake callback, so that
                // frame releases fragments at age zero. At six seconds of
                // encounter time their 375/150-second lifetime is not over.
                s32 ageFrames = kObservationFrames - wakeFrame - 1;
                const auto lifetimeFrames = static_cast<s32>(
                    std::ceil(LegendsBossEruption::kLifetime / static_cast<f64>(kStep)));
                REQUIRE(ageFrames > 0);
                REQUIRE(ageFrames < lifetimeFrames);
                std::vector<Mat4> flying;
                for (const usize object : dormantFragments) {
                    CHECK(imported->adapter().worldObjectVisible(object));
                    flying.push_back(imported->adapter().worldObjectTransform(object));
                }
                imported->bossIntro(*opponents.bosses().position());
                imported->update(0, false);
                for (usize index = 0; index < dormantFragments.size(); ++index) {
                    CHECK(imported->adapter().worldObjectVisible(dormantFragments[index]));
                    CHECK(imported->adapter().worldObjectTransform(dormantFragments[index]) ==
                          flying[index]);
                }
                for (; ageFrames < lifetimeFrames;) {
                    imported->capturePresentation();
                    imported->update(kStep, false);
                    opponents.update(1, kStep, players, {}, events);
                    ++ageFrames;
                    const bool alive =
                        static_cast<f64>(ageFrames) * kStep < LegendsBossEruption::kLifetime;
                    for (const usize object : dormantFragments) {
                        CHECK(imported->adapter().worldObjectVisible(object) == alive);
                    }
                }
            }
            // A second callback cannot restart already expired debris either.
            imported->bossIntro(*opponents.bosses().position());
            imported->update(0, false);
            for (const usize object : dormantFragments) {
                CHECK_FALSE(imported->adapter().worldObjectVisible(object));
            }
            REQUIRE_FALSE(opponents.bosses().targets().empty());
            bool damaged = false;
            for (s32 frame = 0; frame < 180 && !damaged; ++frame) {
                opponents.bosses().hurt(hit);
                damaged = opponents.bosses().view().health < initial;
                if (!damaged) {
                    opponents.update(1, kStep, players, {}, events);
                }
            }
            CHECK(damaged);
            device.draws.clear();
            opponents.bosses().draw(device, Mat4{1}, world.lighting());
            CHECK_FALSE(device.draws.empty());

            // Exercise the same death callback as PlayScene: the native map must
            // receive victory only after the common combatant finishes dying.
            const auto scriptRewards = [&] {
                std::vector<LegendsPickup> result;
                for (const auto& pickup : imported->adapter().pickups()) {
                    if (pickup.nativeIndex >= 0x80000000U) {
                        result.push_back(pickup);
                    }
                }
                return result;
            };
            const auto before = scriptRewards();
            REQUIRE(before.size() == encounter->rewards.size());
            for (const auto& reward : before) {
                CHECK_FALSE(reward.active);
            }
            CHECK_FALSE(imported->travelRequested());
            std::array visitors{&players[0].actor};
            usize victories = 0;
            events.fallen = [&](const Vec3& position) {
                ++victories;
                imported->bossDefeated(position + opponents.bosses().rewardOffset(), visitors);
            };
            hit.damage = 100000;
            for (s32 frame = 0; frame < 1800 && victories == 0; ++frame) {
                if (opponents.bosses().view().alive) {
                    opponents.bosses().hurt(hit);
                }
                imported->update(kStep, false);
                opponents.update(1, kStep, players, {}, events);
            }
            REQUIRE(victories == 1);
            CHECK_FALSE(opponents.bosses().present());
            CHECK_FALSE(imported->travelRequested());
            for (s32 frame = 0; frame < 3; ++frame) {
                opponents.update(1, kStep, players, {}, events);
            }
            CHECK(victories == 1);
            const auto released = scriptRewards();
            for (const auto& reward : released) {
                CHECK(reward.active);
                CHECK_FALSE(reward.collected);
            }
            imported->bossDefeated(encounter->combat.position + Vec3{1}, visitors);
            const auto repeated = scriptRewards();
            REQUIRE(repeated.size() == released.size());
            for (usize i = 0; i < repeated.size(); ++i) {
                CHECK(repeated[i].position == released[i].position);
            }
            CHECK(players[0]
                      .actor.save()
                      .progress()
                      .legendsProgress(LegendsPlatform::N64)
                      .runestones ==
                  0x1fff); // final unsupported permanent reward cannot eat earned runestones
            imported->completeTravel(party);
            CHECK_FALSE(
                party[0].save.progress().legendsProgress(LegendsPlatform::N64).hasCompleted(name));
            const s32 waitFrames = static_cast<s32>(std::ceil(encounter->departureSeconds * 60));
            for (s32 frame = 0; frame < waitFrames - 1; ++frame) {
                imported->update(kStep, false);
            }
            CHECK_FALSE(imported->travelRequested());
            for (s32 frame = 0; frame < 2; ++frame) {
                imported->update(kStep, false);
            }
            REQUIRE(imported->destination());
            CHECK(*imported->destination() == "tower2");
            REQUIRE(imported->departure());
            CHECK_FALSE(imported->departure()->finished);
            for (s32 frame = 0; frame < 121; ++frame) {
                imported->update(kStep, false);
            }
            REQUIRE(imported->departure()->finished);
            party[0].save = players[0].actor.save();
            imported->completeTravel(party);
            CHECK(
                party[0].save.progress().legendsProgress(LegendsPlatform::N64).hasCompleted(name));
            CHECK_FALSE(party[0]
                            .save.progress()
                            .legendsProgress(LegendsPlatform::Dreamcast)
                            .hasCompleted(name));
            opponents.close();
            audio.close();
        }
    }
}
