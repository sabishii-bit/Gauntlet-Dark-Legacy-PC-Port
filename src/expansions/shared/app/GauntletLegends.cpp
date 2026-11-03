#include <algorithm>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Assert.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "expansions/dreamcast/audio/DreamcastStream.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/scenes/LegendsMusic.h"
#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/audio/N64LegendsStream.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/level/LegendsUnlocks.h"
#include "expansions/n64/scenes/LegendsN64Music.h"
#include "expansions/shared/app/LegendsConsoleWords.h"
#include "expansions/shared/characters/LegendsVisit.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsMapCatalog.h"
#include "game/app/Gauntlet.h"
#include "game/console/ConsoleHost.h"
#include "game/console/ConsoleListings.h"

namespace gdl::game {
namespace {
std::unique_ptr<SoundPlayer> openLegendsMusic(const std::filesystem::path& levelDirectory,
                                              LegendsPlatform platform, AudioMixer& mixer,
                                              const GameConfig& config, DevConsole& console) {
    try {
        std::shared_ptr<StreamSource> soundtrack;
        std::string soundtrackName;
        if (platform == LegendsPlatform::N64) {
            auto directory = levelDirectory.lexically_normal();
            if (directory.filename().empty()) {
                directory = directory.parent_path();
            }
            if (const auto song = legendsN64SongForLevel(paths::utf8(directory.filename()))) {
                auto stream = std::make_shared<N64LegendsStream>();
                if (stream->open(directory.parent_path().parent_path() / "audio", *song)) {
                    soundtrack = legends_audio_mix::prepare(std::move(stream),
                                                            legends_audio_mix::Source::Music);
                    soundtrackName = std::format("N64 song {}", *song);
                } else {
                    console.print(
                        "Native N64 music requires the complete, unchanged US audio bank.");
                }
            } else {
                console.print("No native soundtrack binding was found for this N64 directory.");
            }
        } else if (const auto music = dreamcastLegendsMusic(levelDirectory)) {
            auto stream = std::make_shared<DreamcastStream>();
            if (stream->open(*music)) {
                soundtrack = std::move(stream);
                soundtrackName = paths::utf8(music->filename());
            } else {
                console.print("The level's native music stream could not be opened.");
            }
        } else {
            console.print("No verified native soundtrack binding was found for this directory.");
        }
        if (soundtrack) {
            auto sounds = std::make_unique<SoundPlayer>(mixer);
            sounds->setMasterVolume(config.audio.masterVolume);
            sounds->setCategoryVolume(SoundCategory::Music, config.audio.musicVolume);
            sounds->playStream(std::move(soundtrack), true);
            console.print(std::format("Native soundtrack: {}", soundtrackName));
            return sounds;
        }
    } catch (const std::exception& error) {
        console.print(std::format("Music could not start: {}", error.what()));
    }
    return {};
}
} // namespace

const std::vector<LegendsMapEntry>& Gauntlet::foundLegendsMaps(LegendsPlatform platform,
                                                               bool rescan) const {
    auto& found = m_legendsMaps[platform == LegendsPlatform::N64 ? 0 : 1];
    if (rescan || !found.has_value()) {
        found = legendsMaps(platform, m_options.unpackedDirectory, paths::executableDirectory(),
                            std::filesystem::current_path());
    }
    return *found;
}

bool Gauntlet::legendsMapsInstalled(bool rescan) const {
    bool installed = false;
    for (const LegendsPlatform platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        const bool any = !foundLegendsMaps(platform, rescan).empty();
        installed = installed || any;
    }
    return installed;
}

std::optional<std::span<const std::string_view>> Gauntlet::legendsConsoleHold() const {
    if (!m_legendsPreview.isOpen()) {
        return std::nullopt;
    }
    return kLegendsPreviewWords;
}

void Gauntlet::registerLegendsConsole(ConsoleHost& host, ConsoleListings& listings) {
    registerLegendsConsoleWords(
        host, listings,
        {.visiting = [this] { return m_legendsPreview.isOpen() || m_legendsOrigin != nullptr; },
         .installed = [this](bool rescan) { return legendsMapsInstalled(rescan); },
         .mapNames =
             [this](LegendsPlatform platform) {
                 std::vector<std::string> names;
                 for (const LegendsMapEntry& map : foundLegendsMaps(platform, false)) {
                     names.push_back(map.name);
                 }
                 return names;
             },
         .find =
             [this](std::string_view name, std::optional<LegendsPlatform> platform) {
                 return findLegendsMap(name, platform);
             },
         .open = [this](DevConsole& console, LegendsPlatform platform,
                        const std::filesystem::path& directory,
                        bool walk) { consoleLegendsOpen(console, platform, directory, walk); },
         .close = [this](DevConsole& console) { consoleLegendsClose(console); },
         .walkingNative =
             [this] {
                 return m_online == nullptr &&
                        dynamic_cast<const LegendsLevel*>(m_play->world.imported()) != nullptr;
             },
         .unlockTower = [this](DevConsole& console) { consoleLegendsUnlockTower(console); }});
}

void Gauntlet::consoleLegendsUnlockTower(DevConsole& console) {
    auto* native = dynamic_cast<LegendsLevel*>(m_play->world.imported());
    GDL_VERIFY(native != nullptr, "unlock_tower's native form runs in a native world alone");
    if (native->platform() != LegendsPlatform::N64) {
        console.print("Native Dreamcast tower unlocking is not supported yet.");
        return;
    }
    try {
        const auto root = native->directory().parent_path().parent_path();
        const auto program = AssetLocator{root}.find("_program/game.bin");
        if (!program) {
            console.print("The native N64 unlock table is unavailable.");
            return;
        }
        const auto unlocks = LegendsN64Unlocks::decode(readFile(*program));
        s32 changed = 0;
        for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
            if (auto* save = m_play->scene.liveSave(player)) {
                if (unlocks.unlockAll(save->progress().legendsProgress(LegendsPlatform::N64))) {
                    ++changed;
                }
            }
        }
        // Native gates observe the live union on the next gameplay tick.
        // Never reopen the Dark Legacy tower or mutate its crystal/relic bits.
        console.print(std::format("The N64 tower stands open to {} character{}.", changed,
                                  changed == 1 ? "" : "s"));
    } catch (const std::exception& error) {
        console.print(std::format("Native tower unlock failed: {}", error.what()));
    }
}

std::optional<std::pair<LegendsPlatform, std::filesystem::path>>
Gauntlet::findLegendsMap(std::string_view name, std::optional<LegendsPlatform> platform) const {
    for (const LegendsPlatform candidate : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        if (platform.has_value() && *platform != candidate) {
            continue;
        }
        if (const auto directory = legendsMapPath(foundLegendsMaps(candidate, true), name)) {
            return std::pair{candidate, *directory};
        }
    }
    return std::nullopt;
}

void Gauntlet::consoleLegendsClose(DevConsole& console) {
    if (m_play->scene.leaving()) {
        console.print("Wait for the current level transition to finish.");
        return;
    }
    if (!m_legendsPreview.isOpen() && m_legendsOrigin) {
        if (closeLegendsLevel()) {
            console.print("Returned from the Legends level.");
        }
        return;
    }
    if (!m_legendsPreview.isOpen()) {
        console.print("Not in a Legends map.");
        return;
    }
    // Direct render backends do not retain texture leases for queued frames.
    renderDevice().waitIdle();
    commitLegendsProgress();
    m_legendsPreview.close();
    if (m_legendsSounds) {
        m_legendsSounds->stopAll();
        m_legendsSounds.reset();
    }
    m_play->scene.releaseSounds();
    if (const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported())) {
        m_legendsSounds = openLegendsMusic(native->directory(), native->platform(),
                                           m_audio->mixer(), m_config, console);
    }
    window().holdCursor(false);
    resetPlayInput();
    console.print("Returned from the Legends map preview.");
}

void Gauntlet::consoleLegendsOpen(DevConsole& console, LegendsPlatform platform,
                                  const std::filesystem::path& directory, bool playable,
                                  bool completedTravel,
                                  std::optional<std::vector<PartyMember>> visitingParty) {
    const auto* imported = m_play->world.imported();
    const auto nativeDeparture = imported ? imported->departure() : std::nullopt;
    const bool internalTravel =
        completedTravel && imported != nullptr && imported->travelRequested() &&
        m_play->scene.leaving() &&
        (nativeDeparture
             ? nativeDeparture->covered(m_play->scene.transition().covering())
             : m_play->scene.transition().covering() && m_play->scene.departure().finished());
    if (completedTravel && !internalTravel) {
        console.print("The native departure has not finished.");
        return;
    }
    const bool n64 = platform == LegendsPlatform::N64;
    try {
        // Only finishLegendsTravel supplies the private completed-travel flag.
        // A player console command cannot bypass the pending exit animation.
        if (!m_play->scene.isOpen() || (m_play->scene.leaving() && !internalTravel) ||
            m_journey.has_value()) {
            console.print("Enter an offline level before opening a Legends map.");
            return;
        }
        const AssetLocator files{directory};
        const auto world = files.find(n64 ? "worlds.wad" : "worlds.rom");
        const auto models = files.find(n64 ? "objects.wad" : "objects.rom");
        if (!world || !models) {
            console.print("The directory must contain the native worlds and objects files.");
            return;
        }
        const auto worldBytes = readFile(*world);
        const auto modelBytes = readFile(*models);
        const auto placements = files.find("ipr.rom");
        const auto placementBytes = n64 && placements ? readFile(*placements) : std::vector<u8>{};
        auto map = n64 ? formats::LegendsMap::parseN64(worldBytes, modelBytes, placementBytes)
                       : formats::LegendsMap::parseDreamcast(worldBytes, modelBytes);
        if (n64) {
            const auto dynamic = files.find("texdyn.rom");
            const auto dynamicBytes = dynamic ? readFile(*dynamic) : std::vector<u8>{};
            for (auto& texture : formats::decodeLegendsN64Textures(modelBytes, dynamicBytes)) {
                map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                        texture.hasTransparency, std::move(texture.name),
                                        texture.external, texture.width, texture.height});
            }
        }
        if (!n64) {
            if (const auto pixels = files.find("textures.rom")) {
                map.textures =
                    formats::decodeLegendsDreamcastTextures(modelBytes, readFile(*pixels));
            } else {
                map.warnings.emplace_back("textures.rom is missing; showing untextured geometry.");
            }
        }
        auto adapter = LegendsSceneAdapter::load(
            renderDevice(), platform, files.root(), map,
            playable ? static_cast<s32>(m_play->scene.party().size()) : 1);
        console.print(std::format("Native presentation: {} fixtures, {} world texture animations.",
                                  adapter->fixtureCount(), adapter->textureAnimationCount()));
        console.print(std::format("{} placed pickups have verified gameplay mappings.",
                                  adapter->pickups().size()));
        for (const auto& warning : adapter->warnings()) {
            console.print(warning);
        }
        if (m_legendsPreview.isOpen()) {
            renderDevice().waitIdle();
            if (!commitLegendsProgress()) {
                return;
            }
        }
        if (playable) {
            auto party = visitingParty ? std::move(*visitingParty) : m_play->scene.party();
            if (internalTravel) {
                if (const auto* previous = dynamic_cast<const LegendsLevel*>(imported)) {
                    previous->completeTravel(party);
                }
            }
            const auto selected = std::ranges::find_if(
                party, [](const PartyMember& member) { return !member.fallen; });
            if (selected == party.end()) {
                console.print("Select a character before exploring a Legends map.");
                return;
            }
            ClassDataSet classes;
            classes.load(m_options.unpackedDirectory / "pdata");
            LegendsTraversal entrance;
            entrance.open(map, selected->player, selected->save,
                          classes.stats(selected->save.character));
            Vec3 low = entrance.start();
            Vec3 high = low;
            for (const auto& face : map.collision) {
                for (const auto& vertex : face.vertices) {
                    low = glm::min(low, vertex);
                    high = glm::max(high, vertex);
                }
            }
            WorldLocator start;
            start.kind = LocatorKind::Start;
            start.position = entrance.start();
            start.rotation.y = entrance.actor().yaw() - kPi;
            if (n64 && !m_legendsAdvice)
                m_legendsAdvice = std::make_shared<LegendsAdviceState>();
            auto native = std::make_shared<LegendsLevel>(renderDevice(), map, std::move(adapter),
                                                         files.root(), m_options.unpackedDirectory,
                                                         n64 ? m_legendsAdvice : nullptr);
            native->initializeParty(party);
            if (n64 && native->adapter().isTower() && m_legendsOrigin) {
                native->announceProgress(
                    m_legendsTowerParty.empty() ? m_legendsEntryParty : m_legendsTowerParty, party);
            }
            auto next = std::make_unique<PlaySession>();
            LevelRef ref;
            ref.realmId = -1; // native realm IDs must never index the GDL campaign
            ref.realm = std::string(legendsPlatformName(platform));
            ref.name = paths::utf8(native->directory().filename());
            ref.title = ref.name;
            ref.directory = paths::utf8(files.root());
            if (!next->world.loadImported(renderDevice(), m_options.unpackedDirectory, ref,
                                          WorldLayout::fromDecoded({}, {start}, low, high),
                                          entrance.collision(), native)) {
                console.print("The native map could not be bound to gameplay.");
                return;
            }
            auto nextContext = context();
            nextContext.tower = &next->world;
            PlayOptions options;
            options.position = entrance.start();
            options.yaw = entrance.actor().yaw();
            options.welcome = false;
            options.partyCount = m_partyCountOverride;
            options.consoleCheats = m_consoleCheats;
            // Keep an admitted exit phrase alive until the successfully opened
            // destination takes ownership. A console jump still stops it.
            m_play->scene.suspendForImportedLevel(internalTravel);
            if (!next->scene.open(renderDevice(), nextContext, next->world, party, options,
                                  playLinks())) {
                m_play->scene.resumeAfterImportedLevel();
                console.print("The native map's shared gameplay resources could not be loaded.");
                return;
            }
            // A native departure is a normal save boundary too. Prepare/open the
            // destination first so a missing or malformed destination cannot
            // manufacture completion on disk. Preserve assigned save slots in
            // the new live party, not only in this temporary snapshot.
            if (internalTravel || visitingParty) {
                autoSave(party);
                for (const auto& member : party) {
                    next->scene.setSaveSlot(member.player, member.slot);
                }
            }
            renderDevice().waitIdle();
            if (internalTravel) {
                if (auto* previous = dynamic_cast<LegendsLevel*>(m_play->world.imported())) {
                    native->continueSpeechFrom(*previous);
                }
            }
            m_legendsPreview.close();
            if (!m_legendsOrigin) {
                m_legendsEntryParty = party;
                m_legendsTowerParty = party;
                m_legendsOrigin = std::move(m_play);
            }
            if (n64 && native->adapter().isTower()) {
                m_legendsTowerParty = party;
            }
            m_play = std::move(next);
            m_play->scene.releaseSounds();
            m_sounds->setCategoryScale(SoundCategory::Music, m_musicDuck.level());
            m_sounds->setCategoryScale(SoundCategory::Effects, 1);
            setMaxFrameRate(m_config.timing.gameplayFrameRate);
        } else {
            m_legendsPreview.open(renderDevice(), map, std::move(adapter));
            m_play->scene.holdSounds();
        }
        if (m_legendsSounds) {
            m_legendsSounds->stopAll();
            m_legendsSounds.reset();
        }
        m_legendsSounds =
            openLegendsMusic(files.root(), platform, m_audio->mixer(), m_config, console);
        window().stopRumble();
        resetPlayInput();
        console.print(std::format("Legends native map: {} objects.", map.objects.size()));
        if (playable) {
            console.print("Normal party controls, combat, inventory and pause are active.");
            console.print(
                "Escape opens the pause menu. Save normally after returning to keep rewards.");
        } else {
            console.print("Native scene preview. goto <map> without preview walks its collision.");
            console.print(std::string{FreeCamera::kHelp});
        }
        console.print("goto back returns to the level left for it.");
        for (const auto& warning : map.warnings) {
            console.print(warning);
        }
    } catch (const std::exception& error) {
        if (m_play->scene.isOpen() && !m_legendsPreview.isOpen()) {
            m_play->scene.resumeAfterImportedLevel();
        }
        console.print(std::format("Legends map was not opened: {}", error.what()));
    }
}

void Gauntlet::updateLegendsPreview(f64 deltaSeconds) {
    const bool wasOpen = m_console.open();
    m_console.setSession(consoleSession());
    m_console.update(deltaSeconds, input(), m_config.menu.console);
    const bool captures = m_console.open() || wasOpen;
    if (m_legendsPreview.isOpen() && !captures && input().wasKeyPressed(Key::Escape)) {
        renderDevice().waitIdle();
        commitLegendsProgress();
        m_legendsPreview.close();
        if (m_legendsSounds) {
            m_legendsSounds->stopAll();
            m_legendsSounds.reset();
        }
        m_play->scene.releaseSounds();
        if (const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported())) {
            m_legendsSounds = openLegendsMusic(native->directory(), native->platform(),
                                               m_audio->mixer(), m_config, m_console);
        }
        resetPlayInput();
    }
    if (m_legendsPreview.playable()) {
        m_legendsPreview.updatePlayable(input(), static_cast<f32>(deltaSeconds), captures,
                                        m_config);
    } else {
        m_legendsPreview.update(input(), static_cast<f32>(deltaSeconds), captures);
    }
    window().holdCursor(m_legendsPreview.isOpen() && !m_legendsPreview.playable() && !captures &&
                        input().isKeyDown(Key::MouseRight));
    m_sounds->setCategoryScale(SoundCategory::Music,
                               m_legendsPreview.isOpen() ? 0.0f : m_musicDuck.level());
    m_sounds->setCategoryScale(SoundCategory::Effects, m_legendsPreview.isOpen() ? 0.0f : 1.0f);
    m_sounds->update();
    if (m_legendsSounds) {
        m_legendsSounds->update();
    }
}

bool Gauntlet::commitLegendsProgress() {
    if (m_legendsOrigin) {
        const auto visiting = m_play->scene.party();
        std::vector<std::pair<s32, CharacterSave>> changes;
        for (const auto& before : m_legendsEntryParty) {
            if (before.fallen) {
                continue; // that seat waited outside the visit and earned no rewards
            }
            const auto after = std::ranges::find_if(visiting, [&](const PartyMember& member) {
                return member.player == before.player;
            });
            auto* live = m_legendsOrigin->scene.liveSave(before.player);
            if (!live) {
                m_console.print("The native visit could not be returned to its original party.");
                return false;
            }
            // A fallen player can leave the local session. That does not prevent
            // the remaining party returning; no rewards are copied for that seat.
            if (after == visiting.end()) {
                continue;
            }
            auto candidate = *live;
            if (!commitLegendsVisit(candidate, before.save, after->save)) {
                m_console.print("The original character changed; its newer state was preserved.");
                return false;
            }
            changes.emplace_back(before.player, std::move(candidate));
        }
        for (auto& [player, save] : changes) {
            *m_legendsOrigin->scene.liveSave(player) = std::move(save);
            const auto returned = std::ranges::find(visiting, player, &PartyMember::player);
            m_legendsOrigin->scene.setSaveSlot(player, returned->slot);
            m_legendsOrigin->scene.restat(player);
        }
        m_legendsEntryParty = m_legendsOrigin->scene.party();
        return true;
    }
    const auto* traversal = m_legendsPreview.traversal();
    if (!traversal) {
        return true;
    }
    const s32 player = traversal->actor().player();
    auto* live = m_play->scene.liveSave(player);
    if (!live || !m_legendsPreview.transferProgress(*live)) {
        m_console.print("Legends rewards were not applied: the parked character changed during "
                        "this visit. Its newer state was preserved.");
        return false;
    }
    m_play->scene.restat(player);
    return true;
}

bool Gauntlet::closeLegendsLevel(bool keepProgress) {
    if (!m_legendsOrigin || (keepProgress && !commitLegendsProgress())) {
        return false;
    }
    renderDevice().waitIdle();
    m_play = std::move(m_legendsOrigin);
    m_legendsEntryParty.clear();
    m_legendsTowerParty.clear();
    if (m_legendsSounds) {
        m_legendsSounds->stopAll();
        m_legendsSounds.reset();
    }
    m_play->scene.resumeAfterImportedLevel();
    // The level left for the map held the cheats it had then; the visit's go on.
    for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
        m_play->scene.setCheats(seat, m_consoleCheats[static_cast<usize>(seat)]);
    }
    m_sounds->setCategoryScale(SoundCategory::Music, m_musicDuck.level());
    m_sounds->setCategoryScale(SoundCategory::Effects, 1);
    resetPlayInput();
    return true;
}

bool Gauntlet::quitLegendsLevel() {
    const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported());
    if (!native) {
        return closeLegendsLevel(false);
    }
    const auto platform = native->platform();
    const auto tower = legendsTowerPath(platform, native->directory());
    if (!tower) {
        m_console.print("Native Tower files unavailable; keeping the current level open.");
        return false;
    }
    const auto visiting = m_play->scene.party();
    auto restored = m_play->scene.abandonedParty(visiting);
    for (auto& member : restored) {
        if (const auto* current = m_play->scene.liveSave(member.player)) {
            retainLegendsProgress(member.save, *current, platform);
        }
    }
    const auto* before = m_play.get();
    consoleLegendsOpen(m_console, platform, *tower, true, false, std::move(restored));
    // On failure retain the original scene and unsaved awards. Never silently
    // send a native Quit Level to the parked Dark Legacy campaign.
    return m_play.get() != before;
}

void Gauntlet::finishLegendsTravel() {
    const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported());
    if (native && !native->destination()) {
        // Common total-party defeat returns Travel without an admitted native
        // portal. Restore the visit into its own Tower, not the parked GDL map.
        quitLegendsLevel();
        return;
    }
    if (!native || !native->destination()) {
        closeLegendsLevel();
        return;
    }
    // Resolve beside the currently loaded extraction, never through an unrelated
    // installed copy. consoleLegendsOpen builds the replacement before swapping.
    const std::filesystem::path path = native->directory().parent_path() / *native->destination();
    const auto generation = m_play->scene.loadGeneration();
    const auto* previous = m_play.get();
    consoleLegendsOpen(m_console, native->platform(), path, true, true);
    if (m_play.get() == previous && m_play->scene.loadGeneration() == generation) {
        m_play->scene.recoverImportedDeparture();
        m_console.print("Native destination unavailable; this level and its rewards were kept. "
                        "Leave and re-enter the portal to retry, or use Quit Level for its Tower.");
        resetPlayInput();
    }
}

} // namespace gdl::game
