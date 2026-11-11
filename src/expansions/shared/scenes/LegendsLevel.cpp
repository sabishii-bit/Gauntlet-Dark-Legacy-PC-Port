#include "expansions/shared/scenes/LegendsLevel.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "engine/assets/StringTable.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "expansions/dreamcast/camera/LegendsDreamcastCamera.h"
#include "expansions/dreamcast/menu/LegendsDreamcastDialogue.h"
#include "expansions/dreamcast/menu/LegendsDreamcastEntrance.h"
#include "expansions/n64/bosses/LegendsLegend.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/menu/LegendsDialogue.h"
#include "expansions/n64/scenes/LegendsSumnerText.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

LegendsLevel::LegendsLevel(RenderDevice& device, const formats::LegendsMap& map,
                           std::unique_ptr<LegendsSceneAdapter> adapter,
                           const std::filesystem::path& directory,
                           const std::filesystem::path& characterRoot,
                           std::shared_ptr<LegendsAdviceState> advice)
    : m_directory(directory.lexically_normal()),
      m_advice(advice ? std::move(advice) : std::make_shared<LegendsAdviceState>()) {
    if (m_directory.filename().empty()) {
        m_directory = m_directory.parent_path();
    }
    if (!adapter) {
        throw std::invalid_argument("Legends level requires native resources");
    }
    m_world.open(device, map, std::move(adapter));
    if (!map.starts.empty()) {
        m_playerStart = map.starts.front().position;
    }
    m_classes.load(characterRoot / "pdata");
    if (platform() == LegendsPlatform::Dreamcast) {
        const auto nativeRoot = m_directory.parent_path().parent_path();
        m_dreamcastSounds.open(nativeRoot);
        m_dreamcastSpeech.open(nativeRoot);
        m_dreamcastAmbient.reset(
            formats::LegendsDreamcastScene::parse(readFile(m_directory / "worlds.rom")),
            m_world.adapter()->playerCount(),
            [&](std::string_view name) { return m_dreamcastSounds.namedToken(name); });
        if (const auto* movement = m_world.adapter()->dreamcastWorldMotion()) {
            for (const auto& emitter : m_dreamcastAmbient.emitters()) {
                // SetItem binds a sound marker to an animated node within ten
                // units, after the world's initial pose has been installed.
                if (const auto parent = movement->ambientParent(emitter.position)) {
                    m_dreamcastAmbientSources.push_back(
                        {emitter.nativeIndex, parent->first, parent->second});
                }
            }
        }
        const auto dataName =
            formats::dreamcastLegendsWorldDataFile(paths::utf8(m_directory.filename()));
        if (!dataName.empty()) {
            if (const auto data = AssetLocator{nativeRoot}.find(dataName)) {
                const auto bytes = readFile(*data);
                m_dreamcastBoss = legendsDreamcastBoss(bytes, readFile(m_directory / "worlds.rom"),
                                                       paths::utf8(m_directory.filename()));
                if (m_dreamcastBoss) {
                    const auto moves = AssetLocator{nativeRoot / "critters"}.find(
                        m_dreamcastBoss->archive + ".wad");
                    if (moves) {
                        m_dreamcastBossCamera = legendsDreamcastBossCamera(readFile(*moves));
                    }
                    m_dreamcastVisit.load(device, m_world.adapter()->scriptArchive("WIZARD"));
                    m_dreamcastVisit.loadSkins(device, nativeRoot);
                }
                if (const auto scope = formats::dreamcastLegendsFixtureScope(
                        bytes, paths::utf8(m_directory.filename()))) {
                    m_dreamcastArenaConfig = scope->bossCamera;
                    if (m_dreamcastBoss) {
                        m_dreamcastBossSubject.position = m_dreamcastBoss->combat.position;
                        m_dreamcastBossSubject.facing = m_dreamcastBoss->combat.yaw;
                    }
                    m_dreamcastEnemyRoster = scope->enemySounds;
                    m_dreamcastRealm = scope->realmType;
                    m_dreamcastSpeech.setEntry(*scope);
                }
            }
        }
        auto messageStyle = std::make_unique<LegendsDreamcastDialogue>();
        if (messageStyle->load(device, m_directory.parent_path().parent_path())) {
            m_messageStyle = std::move(messageStyle);
        }
        if (m_world.adapter()->isTower()) {
            m_dreamcastSumner.emplace();
        }
    }
    if (platform() == LegendsPlatform::N64) {
        const auto nativeRoot = m_directory.parent_path().parent_path();
        auto messageStyle = std::make_unique<LegendsDialogue>();
        if (messageStyle->load(device, nativeRoot)) {
            m_messageStyle = std::move(messageStyle);
        }
        m_sounds.openN64(nativeRoot);
        m_speech.openN64(nativeRoot);
        m_announcements.load(nativeRoot, m_speech);
        if (const auto placements = AssetLocator{m_directory}.find("ipr.rom")) {
            m_ambient.reset(formats::decodeLegendsN64Ambient(readFile(*placements)));
        }
        if (const auto program = AssetLocator{nativeRoot}.find("_program/game.bin")) {
            const auto bytes = readFile(*program);
            if (const auto enemies = AssetLocator{nativeRoot}.find("enemy.rom")) {
                m_enemySounds = LegendsEnemySounds::decode(bytes, readFile(*enemies));
            }
            if (m_world.adapter()->isTower()) {
                if (const auto overlay = AssetLocator{nativeRoot}.find("_program/Tower.bin")) {
                    m_sumner = LegendsSumner::decode(bytes, readFile(*overlay));
                }
            }
            m_trapDamageScale =
                formats::legendsN64TrapScale(bytes, paths::utf8(m_directory.filename()))
                    .value_or(1);
            if (const auto overlay = AssetLocator{nativeRoot}.find("_program/Boss.bin")) {
                m_boss =
                    legendsN64Boss(paths::utf8(m_directory.filename()), bytes, readFile(*overlay));
                if (m_boss) {
                    m_bossCamera = legendsN64BossCamera(
                        readFile(nativeRoot / m_boss->nativeArchive / "moves.wad"));
                    std::vector<std::string> warnings;
                    auto legend =
                        loadLegendsLegend(nativeRoot, m_boss->consumedLegend, bytes, warnings);
                    m_bossLegend = std::move(legend.archive);
                    m_bossLegendProfile = std::move(legend.profile);
                }
            }
        }
        // e0072b6c loads CHROMESILVER from the native static archive;
        // e004e1a8/e0030584 replace every departing character's material with it.
        const AssetLocator files{m_directory.parent_path().parent_path() / "static"};
        if (const auto objects = files.find("objects.wad")) {
            const auto textures = formats::decodeLegendsN64Textures(readFile(*objects));
            const auto found = std::ranges::find(textures, std::string{"CHROMESILVER"},
                                                 &formats::LegendsN64Texture::name);
            if (found != textures.end() && !found->external && !found->image.pixels.empty()) {
                const TextureDesc description{
                    .width = found->image.width,
                    .height = found->image.height,
                    .wrap = found->clampU ? TextureWrap::ClampToEdge : TextureWrap::Repeat,
                    .wrapV = found->clampV ? TextureWrap::ClampToEdge : TextureWrap::Repeat,
                    .generateMipmaps = true};
                m_departureSkin = device.createTexture(description, found->image.pixels);
            }
        }
    }
}

void LegendsLevel::capturePresentation() {
    m_previousDepartureSeconds = m_departureSeconds;
    m_dreamcastVisit.capturePresentation();
    m_world.adapter()->capturePresentation();
}

void LegendsLevel::update(f32 seconds, bool timeStopped) {
    if (std::isfinite(seconds) && seconds > 0) {
        m_entranceSeconds =
            std::min(m_entranceSeconds + seconds, LegendsDreamcastEntrance::kDurationTicks / 60.0);
    }
    if (m_soundOutput != nullptr) {
        m_speech.update(*m_soundOutput, seconds);
    }
    if (m_dreamcastBoss && m_bossDefeated && !m_departureFailed) {
        const auto event = m_dreamcastVisit.update(seconds);
        if (event.appeared && m_dreamcastBoss->nativeKind < 32) {
            if (m_dreamcastArenaCamera) {
                m_dreamcastArenaCamera->focusOn(m_dreamcastVisit.attention());
            } else if (m_dreamcastCamera) {
                m_dreamcastCamera->focusOn(m_dreamcastVisit.attention());
            }
        }
        if (event.finished) {
            m_destination = "levelt1";
        }
    }
    if (m_bossDefeated && m_boss && !m_destination && !m_departureFailed &&
        std::isfinite(seconds) && seconds > 0) {
        m_bossDepartureSeconds += seconds;
        if (m_bossDepartureSeconds >= m_boss->departureSeconds) {
            m_destination = m_boss->destination;
        }
    }
    if (m_destination && platform() == LegendsPlatform::N64 && std::isfinite(seconds) &&
        seconds > 0) {
        m_departureSeconds = std::min(m_departureSeconds + seconds,
                                      static_cast<f64>(kLegendsPortalDepartureSeconds));
    }
    // Scenery clocks are independent of Stop Time's enemy freeze, as in GDL.
    m_world.adapter()->update(seconds);
    if (const auto* ceremony = adapter().towerCeremony(); ceremony && ceremony->pending() &&
                                                          !ceremony->active() && m_camera &&
                                                          !m_camera->introActive()) {
        // Player callbacks are deliberately held by opponentsReady during this
        // script. Start on the independent world clock, never behind that hold. The cut
        // follows this update's scenery tick, as the party update's start does: state
        // 0x12's handler (game.bin e0045208) runs one dispatcher case an update, so the
        // first Select comes a whole 30 Hz update after the cut. The shots turn about the
        // standing party's mean feet (e086007c): those the visit opened with, else, for a
        // ceremony queued in play, those the camera follows.
        const Vec3 party =
            std::exchange(m_ceremonyParty, std::nullopt).value_or(m_camera->partyFeet());
        m_world.adapter()->startTowerCeremonies(m_camera->camera(), m_camera->attention(), party);
    }
    m_world.adapter()->updateTraps(seconds, timeStopped);
    if (m_collision) {
        m_world.adapter()->updateBossRewards(seconds, *m_collision);
    }
    for (auto& blast : m_blasts) {
        blast.step(seconds);
    }
    std::erase_if(m_blasts, [](const auto& blast) { return !blast.alive(); });
}

void LegendsLevel::bindCollision(WorldCollision& collision) const {
    m_collision = &collision;
    m_world.adapter()->bindCollision(collision);
}

bool LegendsLevel::bossActivationReady(const Vec3& position) const {
    if (platform() != LegendsPlatform::Dreamcast) {
        return true;
    }
    const auto* motion = adapter().dreamcastWorldMotion();
    if (!motion || !m_collision) {
        return false;
    }
    const auto floor = m_collision->floorAt(position, 4, 1000, 0, WorldCollision::kBossSide);
    return floor && motion->activatesBoss(floor->object);
}

void LegendsLevel::syncCollision(WorldCollision& collision) const {
    m_world.adapter()->syncCollision(collision);
}

std::optional<ImportedDeparture> LegendsLevel::departure(f32 alpha) const {
    if (m_dreamcastBoss && m_bossDefeated &&
        (m_destination ||
         (m_dreamcastVisit.state().stage() == LegendsDreamcastBossCeremony::Stage::Leaving &&
          m_dreamcastVisit.state().ticksLeft() <= 20))) {
        // DoGoodWizard's end-level timer never enters the portal's sinking state.
        ImportedDeparture result;
        result.finished = m_dreamcastVisit.state().finished();
        result.skin = m_dreamcastVisit.skin();
        result.skinAlpha = TreeModel::SkinAlpha::Dissolve;
        result.keepVisible = true;
        result.transportSound = false;
        return result;
    }
    if (!m_destination || platform() != LegendsPlatform::N64) {
        return std::nullopt;
    }
    const f64 elapsed = alpha < 0 || !std::isfinite(alpha)
                            ? m_departureSeconds
                            : std::lerp(m_previousDepartureSeconds, m_departureSeconds,
                                        static_cast<f64>(std::clamp(alpha, 0.0f, 1.0f)));
    const auto sample = legendsPortalDeparture(static_cast<f32>(elapsed));
    if (const auto tower = m_world.adapter()->towerDeparture()) {
        ImportedDeparture result;
        result.finished = sample.finished;
        result.keepVisible = true;
        result.transportSound = false;
        if (*tower == LegendsTowerPortalKind::Shards) {
            // Tower e08641c4 holds substate4 (not transportation0x12),
            // whitening the view through its two-second fog transition.
            // The room whitens by actual projected-depth fog, not a flat fade.
            // Only the completed fully white frame covers the HUD for handoff.
            result.fog = legendsCathedralFog(static_cast<f32>(elapsed));
            result.whiteFade = sample.finished ? 1.0f : 0.0f;
        }
        // The rune exit's native e004e1a8 normally spins/shrinks. Keep its
        // requested stationary presentation, without altering ordinary exits.
        return result;
    }
    ImportedDeparture result;
    result.yaw = sample.yaw;
    result.scale = sample.scale;
    result.skin = m_departureSkin.get();
    result.finished = sample.finished;
    return result;
}

std::vector<PointLight> LegendsLevel::pointLights() const {
    return m_world.adapter()->towerPortalLights();
}

bool LegendsLevel::cancelDeparture() {
    if (!m_destination) {
        return false;
    }
    m_destination.reset();
    m_departureFailed = true;
    m_departureSeconds = m_previousDepartureSeconds = 0;
    m_exitSpeechPlayed = false;
    m_world.adapter()->cancelPortalTravel();
    // A defeated boss has no re-enterable exit fixture. Do not repeatedly load
    // the same broken destination from its victory timer; Quit Level remains
    // available to return to this platform's Tower without losing discoveries.
    return true;
}

void LegendsLevel::draw(RenderDevice& device, const Mat4& clip, const WorldCamera& view,
                        TreeModel::Pass pass, f32 alpha) const {
    m_world.drawWorld(device, clip, view, pass, alpha);
    m_dreamcastVisit.draw(device, clip, view, pass, alpha);
}

void LegendsLevel::drawEntranceTitle(Canvas& canvas, f32 width) const {
    if (platform() == LegendsPlatform::Dreamcast && m_messageStyle) {
        LegendsDreamcastEntrance::draw(canvas, m_messageStyle->menuText(),
                                       adapter().dreamcastTitle(), width,
                                       static_cast<s32>(m_entranceSeconds * 60));
        m_dreamcastVisit.drawCaption(canvas, m_messageStyle->menuText(), width);
    }
    if (platform() == LegendsPlatform::N64 && m_strings) {
        // Only the N64 dialogue style draws the ceremony's captions.
        const auto* dialogue = dynamic_cast<const LegendsDialogue*>(m_messageStyle.get());
        const auto* ceremony = adapter().towerCeremony();
        if (dialogue && ceremony && ceremony->active()) {
            // The captions are Sumner's lines of the string table, like his menu and replies.
            if (const auto caption = ceremony->caption()) {
                if (const auto key = legendsN64CeremonyCaptionKey(*caption);
                    !key.empty() && m_strings->has(key)) {
                    dialogue->drawCeremonyCaption(canvas, m_strings->get(key),
                                                  ceremony->captionOpacity(), width);
                }
            }
        }
    }
}

void LegendsLevel::resetCamera(std::span<const CameraSubject> subjects) {
    const auto markers = adapter().dreamcastCameraMarkers();
    if (platform() == LegendsPlatform::Dreamcast && adapter().isTower() && !subjects.empty() &&
        !markers.empty()) {
        m_dreamcastCamera.reset();
        m_dreamcastTowerCamera.emplace();
        m_dreamcastTowerCamera->reset(subjects, markers.front().position);
    } else if (const auto& config = adapter().dreamcastCameraConfig();
               config && !subjects.empty()) {
        m_dreamcastTowerCamera.reset();
        m_dreamcastRange = {config->radiusMin, config->radiusMax, config->minPitch,
                            config->boundsMin, config->boundsMax};
        m_dreamcastCamera.emplace();
        m_dreamcastCamera->reset(subjects, adapter().dreamcastCameraMarkers(), m_dreamcastRange,
                                 !m_cameraReady);
        if (m_dreamcastBoss && m_dreamcastArenaConfig) {
            m_dreamcastArenaCamera.emplace();
            m_dreamcastArenaCamera->reset(m_dreamcastBossSubject, subjects, *m_dreamcastArenaConfig,
                                          m_dreamcastRange, markers);
        }
        // BCAM follows the party while the boss waits below the floor, then
        // admits its live attention at START. The authored entrance pan has
        // presentation priority while both camera controllers advance.
        m_cameraReady = true;
    }
    if (const auto& config = m_world.adapter()->cameraConfig(); config && !subjects.empty()) {
        m_camera.emplace();
        m_camera->reset(*config, subjects, !m_cameraReady);
        m_cameraReady = true;
    }
}

void LegendsLevel::followCamera(std::span<const CameraSubject> subjects, f32 seconds) {
    if (m_destination) {
        // Departure changes the player's presentation pose, not the camera
        // target. Keep the admitted view while Dreamcast's common transport
        // sinks the party, just as for the native N64 transport sequence.
        return;
    }
    if (m_dreamcastTowerCamera && !subjects.empty()) {
        if (std::isfinite(seconds) && seconds >= 0) {
            m_cameraTickSeconds = seconds;
        }
        m_dreamcastTowerCamera->update(subjects, seconds);
    }
    if (m_dreamcastCamera && !subjects.empty()) {
        if (std::isfinite(seconds) && seconds >= 0) {
            m_cameraTickSeconds = seconds;
        }
        m_dreamcastCamera->update(subjects, seconds);
    }
    if (m_dreamcastArenaCamera && !subjects.empty()) {
        m_dreamcastArenaCamera->update(subjects, seconds);
    }
    if (m_camera && !subjects.empty()) {
        m_cameraTickSeconds = seconds;
        m_camera->update(subjects, seconds);
    }
}

void LegendsLevel::trackBoss(std::optional<Vec3> floorAnchor) {
    if (m_dreamcastArenaCamera) {
        if (floorAnchor) {
            auto subject = m_dreamcastBossSubject;
            subject.position = *floorAnchor;
            subject.attentionOffset =
                Vec3{0, m_dreamcastBossCamera ? m_dreamcastBossCamera->floorOffset : 0, 0};
            if (m_dreamcastBossCamera) {
                subject.radius = m_dreamcastBossCamera->radius;
            }
            subject.engaged = true;
            trackBoss(subject);
        } else {
            m_dreamcastArenaCamera->freeze();
        }
    }
    if (m_camera && m_bossCamera) {
        if (floorAnchor) {
            floorAnchor->y += m_bossCamera->floorOffset + m_bossCamera->attentionHeight;
        }
        m_camera->trackBoss(floorAnchor, m_bossCamera->radius);
    }
}

void LegendsLevel::trackBoss(const BossCameraSubject& subject) {
    if (platform() != LegendsPlatform::Dreamcast) {
        if (subject.engaged) {
            trackBoss(subject.position);
        }
        return;
    }
    m_dreamcastBossSubject = subject;
    if (m_dreamcastBossCamera) {
        // The common combatant publishes its floor root and model offset.
        // Dreamcast ProcessCritter then adds TYPE+A8 to that model root for
        // actor+0x40, the sphere used by BossCameraUpdate8c082d20.
        m_dreamcastBossSubject.attentionOffset.y += m_dreamcastBossCamera->attentionHeight;
    }
    if (m_dreamcastArenaCamera) {
        m_dreamcastArenaCamera->trackBoss(m_dreamcastBossSubject);
    }
}

std::optional<WorldCamera> LegendsLevel::camera(f32 alpha) const {
    if (const auto* ceremony = adapter().towerCeremony(); ceremony && ceremony->active()) {
        return ceremony->camera(alpha);
    }
    if (m_dreamcastTowerCamera) {
        const bool completed = m_destination || alpha < 0 || !std::isfinite(alpha);
        const f32 extra = completed ? 0 : std::clamp(alpha, 0.0f, 1.0f) * m_cameraTickSeconds;
        // The native radial tower branch (8c07e580..8c07ef88) does not cast
        // world sight rays. Near-plane probes hit the ramp below the attention
        // point and collapse its authored view, even when the centre is clear.
        return completed ? m_dreamcastTowerCamera->camera() : m_dreamcastTowerCamera->sample(extra);
    }
    if (m_dreamcastArenaCamera && (!m_dreamcastCamera || !m_dreamcastCamera->introActive())) {
        const bool completed = m_destination || alpha < 0 || !std::isfinite(alpha);
        return completed ? m_dreamcastArenaCamera->camera()
                         : m_dreamcastArenaCamera->sample(std::clamp(alpha, 0.0f, 1.0f) *
                                                          m_cameraTickSeconds);
    }
    if (m_dreamcastCamera) {
        const bool completed = m_destination || alpha < 0 || !std::isfinite(alpha);
        // Dreamcast's authored transmitter branch bypasses world sight rays.
        // Applying GDL's generic clipping here collapses the bridge camera on
        // B1ROPE/B1POST, even though its authored view is clear of those rails.
        return completed
                   ? m_dreamcastCamera->camera()
                   : m_dreamcastCamera->sample(std::clamp(alpha, 0.0f, 1.0f) * m_cameraTickSeconds);
    }
    if (!m_camera) {
        return {};
    }
    return alpha < 0 || (m_destination && platform() == LegendsPlatform::N64)
               ? m_camera->camera()
               : m_camera->sample(std::clamp(alpha, 0.0f, 1.0f) * m_cameraTickSeconds);
}

std::optional<Vec3> LegendsLevel::cameraAttention() const {
    if (const auto* ceremony = adapter().towerCeremony(); ceremony && ceremony->active()) {
        return ceremony->attention();
    }
    if (m_dreamcastTowerCamera) {
        return m_dreamcastTowerCamera->attention();
    }
    if (m_dreamcastArenaCamera && (!m_dreamcastCamera || !m_dreamcastCamera->introActive())) {
        return m_dreamcastArenaCamera->attention();
    }
    if (m_dreamcastCamera) {
        return m_dreamcastCamera->attention();
    }
    return m_camera ? std::optional<Vec3>{m_camera->attention()} : std::nullopt;
}

f32 LegendsLevel::horizontalFov(f32 preferred) const {
    return platform() == LegendsPlatform::Dreamcast && adapter().isTower()
               ? std::max(preferred, LegendsDreamcastTowerCamera::kHorizontalFov)
               : preferred;
}

bool LegendsLevel::opponentsReady() const {
    // The native opening camera holds enemy admission, not just player input.
    // Rendering/sampling cannot release it: only the fixed camera update can.
    const auto* ceremony = adapter().towerCeremony();
    return (ceremony == nullptr || (!ceremony->pending() && !ceremony->active())) &&
           (platform() != LegendsPlatform::Dreamcast || adapter().isTower() ||
            !adapter().dreamcastCameraConfig() ||
            (m_dreamcastCamera && !m_dreamcastCamera->introActive())) &&
           (!adapter().cameraConfig() || (m_camera && !m_camera->introActive()));
}

Vec3 LegendsLevel::resolveMovement(const Vec3& from, const Vec3& to, f32 radius, f32 height) const {
    const Vec3 admitted =
        m_dreamcastTowerCamera ? m_dreamcastTowerCamera->constrainMovement(from, to) : to;
    return m_world.adapter()->resolveMovement(from, admitted, radius, height);
}

void LegendsLevel::initializeParty(std::span<const PartyMember> party) {
    std::vector<const CharacterSave*> saves;
    for (const auto& member : party) {
        if (!member.fallen) {
            saves.push_back(&member.save);
        }
    }
    m_world.adapter()->synchronizeProgress(saves);
    updateExitReminder(saves);
}

void LegendsLevel::completeTravel(std::span<PartyMember> party) const {
    if (!m_destination || adapter().isTower()) {
        return;
    }
    if (platform() == LegendsPlatform::N64 || (m_dreamcastBoss && m_bossDefeated)) {
        const auto completed = departure();
        if (!completed || !completed->finished) {
            return;
        }
    }
    // Ordinary Dreamcast pads use PlayScene's common PortalDeparture instead
    // of the boss ceremony's native override. The app verifies that animation and its
    // covering transition before this callback; a null override is not failure.
    // Native e006bd0c/e006bd84 records the departing stage for the living party,
    // never the destination. This is called only by the app's completed travel
    // route; developer map commands neither call it nor manufacture completion.
    const auto identity = LegendsProgress::mapIdentity(paths::utf8(m_directory.filename()));
    if (!identity) {
        return;
    }
    for (auto& member : party) {
        if (!member.fallen && member.player >= 0 && member.player < 4 &&
            member.save.character >= 0 &&
            static_cast<usize>(member.save.character) < member.save.classes.size()) {
            member.save.progress().legendsProgress(platform()).recordCompleted(*identity);
        }
    }
}

void LegendsLevel::updateParty(std::span<PlayerActor*> players, f32 seconds,
                               bool activatePowerups) {
    if (m_destination) {
        return;
    }
    if (m_dreamcastBoss && m_bossDefeated) {
        std::vector<Vec3> positions;
        for (const auto* actor : players) {
            if (actor) {
                positions.push_back(actor->position());
            }
        }
        m_dreamcastVisit.updatePlayers(positions);
    }
    std::vector<LegendsPortalVisitor> visitors;
    std::vector<CharacterSave*> recipients;
    std::vector<const CharacterSave*> progress;
    for (auto* actor : players) {
        if (actor) {
            recipients.push_back(&actor->save());
            progress.push_back(&actor->save());
        }
    }
    m_world.adapter()->synchronizeProgress(progress);
    if (const auto* ceremony = adapter().towerCeremony();
        ceremony && (ceremony->pending() || ceremony->active())) {
        updateSumner(players, seconds, false);
        return;
    }
    bool readScroll = false;
    m_trapArmor.fill(0);
    for (auto* actor : players) {
        if (!actor) {
            continue;
        }
        // The character's native effect duration is applied by the shared powerup
        // pipeline; pickup validation itself stays in the native-map item adapter.
        const auto* stats = m_classes.stats(actor->save().character);
        if (stats && actor->player() >= 0 && actor->player() < 4) {
            m_trapArmor[static_cast<usize>(actor->player())] =
                static_cast<f32>(characterStats(*stats, actor->save()).armor());
        }
        if (!readScroll) {
            m_world.adapter()->collect(
                *actor, stats ? stats->powerupTime : 1, activatePowerups,
                [&](const LegendsPickup& pickup, const ItemTaking& taking) {
                    readScroll |= !pickup.scrollPages.empty();
                    auto presentation = taking;
                    if (platform() == LegendsPlatform::Dreamcast && pickup.award &&
                        pickup.award->kind == LegendsRelic::Obelisk) {
                        // The Dreamcast fixture already queued S_OBELISKPKUP.
                        // Keep the shared HUD receipt, not its GameCube rune cue.
                        presentation.sound = {};
                    }
                    if (platform() == LegendsPlatform::Dreamcast && pickup.award &&
                        pickup.award->kind == LegendsRelic::Runestone && taking.took()) {
                        // do_got_it 8c0aef72 unions the admitted party's native
                        // rune bits after the award, then counts only bits0..12.
                        u64 runes = 0;
                        for (const auto* recipient : recipients) {
                            if (recipient && recipient->character >= 0 &&
                                recipient->character < kClassCount) {
                                runes |= recipient->progress()
                                             .legendsProgress(LegendsPlatform::Dreamcast)
                                             .runestones;
                            }
                        }
                        m_dreamcastRuneAnnouncements.push_back(
                            static_cast<u32>(std::popcount(runes & 0x1fffU)));
                        m_opponentSounds.push_back(0x24); // Native S_PICKUPRUNE.
                        presentation.sound = {};
                    }
                    m_pickups.push_back({actor->player(), pickup.award ? ItemOffer{} : pickup.offer,
                                         pickup.name, presentation, pickup.scrollPages});
                },
                recipients);
        }
        visitors.push_back({static_cast<u8>(actor->player()), actor->position(), true,
                            actor->radius(), actor->height(), actor->floorObject(),
                            PowerupEffects::of(actor->save().progress().inventory).levitating()});
    }
    m_world.adapter()->synchronizeProgress(progress);
    if (platform() == LegendsPlatform::Dreamcast && m_collision) {
        m_world.adapter()->syncCollision(*m_collision);
    }
    m_visitors = visitors;
    updateExitReminder(progress);
    readScroll |= updateSumner(players, seconds, readScroll);
    // e0071530 drops calls while its advice modal is occupied without spending
    // the once flag. PlayScene supplies only standing, nondeparted living actors.
    if (m_world.adapter()->takeDeathAdvice() && !readScroll && !m_dialogueOpen &&
        platform() == LegendsPlatform::N64 && m_speech.deathAdvice()) {
        const auto recipient = std::ranges::find_if(players, [&](const PlayerActor* actor) {
            return actor != nullptr && actor->player() >= 0 && actor->player() < 4 &&
                   experienceLevel(actor->save().experience()) <=
                       m_speech.deathAdvice()->maximumLevel;
        });
        if (recipient != players.end() &&
            m_advice->admitDeath(experienceLevel((*recipient)->save().experience()),
                                 m_speech.deathAdvice()->maximumLevel)) {
            // Native advice8 opens an authored parchment and voices 4028.
            // Keep the actual input seat, never the party-vector index.
            m_pickups.push_back(
                {(*recipient)->player(), {}, {}, {}, {m_speech.deathAdvice()->text}});
            m_deathAdviceSpeechPending = true;
            readScroll = true;
        }
    }
    if (!readScroll && !(m_dreamcastBoss && m_bossDefeated)) {
        m_world.adapter()->touchTraps(visitors);
        m_world.adapter()->touchSwitches(visitors);
        m_destination = m_world.adapter()->tickPortals(visitors, seconds);
    }
}

std::vector<ImportedPickup> LegendsLevel::takePickups() {
    return std::exchange(m_pickups, {});
}

std::span<const ImportedGenerator> LegendsLevel::importedGenerators() const {
    return m_world.adapter()->generators();
}

void LegendsLevel::updateGenerators(std::span<const ImportedGeneratorState> states) {
    m_world.adapter()->updateGenerators(states);
}

void LegendsLevel::supportOpponents(std::span<const s32> floorOwners) {
    m_world.adapter()->supportSwitches(floorOwners);
}

std::vector<ImportedTarget> LegendsLevel::targets() const {
    auto result = adapter().sceneryTargets();
    const auto deaths = adapter().deathTargets();
    result.insert(result.end(), deaths.begin(), deaths.end());
    for (const auto& target : m_world.adapter()->containerTargets()) {
        // A container has no authored breakable faces of its own.
        result.push_back({target.id,
                          target.base,
                          target.radius,
                          target.height,
                          target.breakable,
                          !target.breakable,
                          {}});
    }
    const auto arena = adapter().arenaDamageTargets();
    result.insert(result.end(), arena.begin(), arena.end());
    return result;
}

bool LegendsLevel::strike(s32 id, f32 power, u32 flags, s32 player) {
    if (id >= LegendsScenery::kTargetBase) {
        const bool hit = m_world.adapter()->strikeScenery(id, power, flags);
        if (hit && m_collision) {
            m_world.adapter()->syncCollision(*m_collision);
        }
        return hit;
    }
    if (id >= kImportedArenaBase) {
        return m_world.adapter()->strikeArenaRock(static_cast<usize>(id - kImportedArenaBase),
                                                  power);
    }
    if (id >= LegendsDeath::kTargetBase) {
        return m_world.adapter()->strikeDeath(id, power);
    }
    return m_world.adapter()->strikeContainer(id, power, flags, player);
}

std::vector<Obstacle> LegendsLevel::obstacles() const {
    auto result = m_world.adapter()->containerObstacles();
    const auto arena = adapter().arenaObstacles();
    result.insert(result.end(), arena.begin(), arena.end());
    const auto deaths = adapter().deathObstacles();
    result.insert(result.end(), deaths.begin(), deaths.end());
    return result;
}

bool LegendsLevel::strikeWorldObject(s32 object, f32 power, u32 flags, s32 /*player*/) {
    const bool hit = m_world.adapter()->strikeWorldObject(object, power, flags);
    if (hit && m_collision) {
        m_world.adapter()->syncCollision(*m_collision);
    }
    return hit;
}

} // namespace gdl::game
