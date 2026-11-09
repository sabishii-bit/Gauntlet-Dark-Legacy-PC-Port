#include <algorithm>
#include <utility>

#include "engine/core/Log.h"
#include "engine/platform/Paths.h"

#include "expansions/shared/scenes/LegendsLevel.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/MissileTarget.h"

namespace gdl::game {

std::vector<LegendsFixtureEvent> LegendsSceneAdapter::takeFixtureEvents() {
    return std::exchange(m_fixtureEvents, {});
}

std::vector<LegendsSoundEvent> LegendsSceneAdapter::takeSounds() {
    return std::exchange(m_soundEvents, {});
}

void LegendsLevel::affectArea(const ImportedAreaDamage& apply) {
    // New blasts are authored by fixture strikes, including chains caused by a
    // prior blast. Their clocks/hit lists remain native; health is always GDL's.
    const auto admit = [&] {
        for (auto& event : m_world.adapter()->takeFixtureEvents()) {
            m_blasts.emplace_back(event, m_trapDamageScale);
        }
    };
    admit();
    // admit() grows m_blasts at the end of each pass, so the index is re-checked.
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (usize i = 0; i < m_blasts.size(); ++i) {
        auto& blast = m_blasts[i];
        const auto area = blast.step(0);
        const auto& event = blast.event();
        const bool poison = event.kind == LegendsFixtureEvent::Kind::Poison;
        if (area && area->radius > 0) {
            for (const auto& visitor : m_visitors) {
                if (const auto damage = blast.touch(visitor)) {
                    const Vec3 direction = visitor.position - event.position;
                    // Dreamcast StartExplosion 8c0d6e00 passes DMG_POISONGAS
                    // (0x800), not DMG_LOW. Adding the latter made Levitation
                    // discard a cloud that still overlaps the player's body.
                    // Keep the independently decoded N64 contact policy intact.
                    const u32 impactFlags =
                        poison && platform() == LegendsPlatform::Dreamcast
                            ? damage::kGas
                            : damage::kLow | (poison ? 0U : damage::kFire | damage::kKnockDown);
                    m_hazardHits.push_back(
                        {visitor.player, *damage, false, poison, {impactFlags, direction}});
                }
            }
            if (apply) {
                // Native flags use a different layout. Do not forward them to
                // GDL's fire/explosion/knockdown/poison damage dispatcher.
                apply(area->position, area->radius, area->damage, poison ? 0x800U : 0x421U,
                      blast.reachedOpponents());
            }
            // Native PTrap/PCon thresholds use physical damage only. In
            // particular, poison can ignite an explosive (threshold 1) without
            // destroying wooden barrels (threshold 6) with its elemental part.
            for (const auto& target : targets()) {
                auto& reached = blast.reachedFixtures();
                if (!target.valid() || !target.damageable ||
                    std::ranges::find(reached, target.id) != reached.end()) {
                    continue;
                }
                const MissileTarget body{target.id, target.base, target.radius, target.height,
                                         target.surface};
                if (body.touches(area->position, area->radius)) {
                    reached.push_back(target.id);
                    strike(target.id,
                           blast.physicalDamageAt(target.base + Vec3{0, target.height * .5f, 0}),
                           0x400U, event.player);
                }
            }
        }
        admit(); // May reallocate m_blasts: no reference above survives this point.
    }
}

std::vector<ImportedHazardHit> LegendsLevel::takeHazardHits() {
    for (const auto& hit : m_world.adapter()->takeTrapHits()) {
        if (hit.player < m_trapArmor.size()) {
            const bool nativeArmor = platform() == LegendsPlatform::N64;
            m_hazardHits.push_back(
                {hit.player,
                 nativeArmor ? legendsTrapDamage(hit.damage, m_trapArmor[hit.player]) : hit.damage,
                 hit.pierces, false, hit.impact, nativeArmor});
        }
    }
    return std::exchange(m_hazardHits, {});
}

bool LegendsLevel::playOpponentSound(std::string_view name, const Vec3& /*position*/) {
    if (platform() == LegendsPlatform::Dreamcast) {
        if (const auto token = m_dreamcastSounds.namedToken(name))
            m_opponentSounds.push_back(*token);
        else if (!name.empty() && m_dreamcastSounds.opened() &&
                 std::ranges::find(m_missingOpponentSounds, name) ==
                     m_missingOpponentSounds.end()) {
            m_missingOpponentSounds.emplace_back(name);
            log::warn("Native Dreamcast opponent cue has no authored routing: {}", name);
        }
        return true;
    }
    if (platform() != LegendsPlatform::N64) {
        return false;
    }
    if (name == "S_LOBBER_BOMB") {
        // e005cabc assigns bomb contact action 1; e00784d8 dispatches that
        // action to the global 0x57 cue before creating the fire burst.
        m_opponentSounds.push_back(0x57U);
        return true;
    }
    if (name == "S_SUICIDE_BOMB") {
        // e005ec28 emits 0x93 except in realm 7, where e0074eec's poison
        // burst emits 0x3c. Use the same realm policy as the damaging cloud.
        const bool poison = suicideUsesGas().value_or(false);
        m_opponentSounds.push_back(poison ? 0x3cU : 0x93U);
        return true;
    }
    if (!m_boss) {
        return false;
    }
    if (const auto token = m_sounds.namedToken(name)) {
        // Boss.bin e0881b84 calls the non-spatial e007a25c with priority 8.
        // Its native effect owns pan and lifetime; GC distance curves do not apply.
        m_opponentSounds.push_back(*token);
    } else if (!name.empty() && m_sounds.opened() &&
               std::ranges::find(m_missingOpponentSounds, name) == m_missingOpponentSounds.end()) {
        m_missingOpponentSounds.emplace_back(name);
        log::warn("Native N64 boss cue has no authored routing: {}", name);
    }
    return true;
}

void LegendsLevel::playSounds(SoundPlayer& output, const Vec3& listener) {
    if (m_soundOutput && m_soundOutput != &output) {
        stopSounds();
    }
    m_soundOutput = &output;
    m_dreamcastVisit.playSounds(output, m_dreamcastSounds);
    // e005d734 admits token 1 once on contact; its native macro owns the loop.
    // Re-entering or resuming admits a fresh voice, leaving contact cancels it.
    if (m_deathDrainContact && m_deathDrainSound == kNoSound) {
        m_deathDrainSound = m_sounds.play(output, 1);
    }
    m_speech.update(output, 0);
    if (platform() == LegendsPlatform::Dreamcast) {
        if (m_entrySpeechPending && m_cameraReady &&
            (!m_entrySpeechAwaitQueue || !m_dreamcastSpeech.playing(output))) {
            m_entrySpeechAwaitQueue = false;
            m_entrySpeechPending = false;
            m_dreamcastSpeech.enqueue(output, m_dreamcastSpeech.entryCue());
        }
        for (const u32 count : std::exchange(m_dreamcastRuneAnnouncements, {}))
            m_dreamcastSpeech.enqueue(output, LegendsDreamcastSpeech::runestoneCue(count));
    }
    if (platform() == LegendsPlatform::N64 && m_entrySpeechPending && m_cameraReady &&
        (!m_entrySpeechAwaitQueue || (!m_speech.playing(output) && m_speech.pendingCount() == 0))) {
        m_entrySpeechAwaitQueue = false;
        m_entrySpeechPending = false;
        if (const auto cue = m_speech.entryCue(paths::utf8(m_directory.filename()))) {
            m_speech.enqueue(output, *cue);
        }
    }
    for (const auto identity : m_world.adapter()->takeObeliskSpeech()) {
        if (const auto cue = m_speech.obeliskCue(identity)) {
            m_speech.enqueue(output, *cue);
        }
    }
    if (std::exchange(m_deathAdviceSpeechPending, false) && m_speech.deathAdvice()) {
        m_speech.enqueue(output, m_speech.deathAdvice()->speech);
    }
    if (m_destination && !m_exitSpeechPlayed) {
        m_exitSpeechPlayed = true;
        if (m_exitReminder) {
            // e004e1a8 calls e00649dc once as the party begins departing.
            // The native stage override, when present, has priority over the
            // ordinary pending rune/obelisk reminder (e0062ca0/e00649dc).
            m_speech.enqueue(output, *m_exitReminder);
        }
    }
    m_ambientMusicDuck.reset();
    if (platform() == LegendsPlatform::Dreamcast) {
        const auto sourcePosition = [&](u32 index) -> std::optional<Vec3> {
            const auto binding =
                std::ranges::find(m_dreamcastAmbientSources, index, &AmbientSource::nativeIndex);
            const auto* movement = m_world.adapter()->dreamcastWorldMotion();
            if (binding == m_dreamcastAmbientSources.end() || !movement)
                return {};
            return Vec3{movement->transform(binding->owner) * Vec4{binding->position, 1}};
        };
        const auto desired =
            m_destination
                ? std::optional<LegendsDreamcastAmbient::Request>{}
                : m_dreamcastAmbient.request(cameraAttention().value_or(listener), sourcePosition);
        if (!desired || m_dreamcastAmbientIndex != desired->nativeIndex) {
            output.stop(m_dreamcastAmbientHandle);
            m_dreamcastAmbientHandle = kNoSound;
            m_dreamcastAmbientIndex.reset();
        }
        if (desired) {
            // AudioSecretProc retries finite cues after EOF while in range;
            // decoded sequences retain their own loop points and envelopes.
            if (!output.isPlaying(m_dreamcastAmbientHandle)) {
                m_dreamcastAmbientHandle =
                    m_dreamcastSounds.play(output, desired->token, desired->volume);
                m_dreamcastAmbientIndex = desired->nativeIndex;
            } else {
                output.setVolume(m_dreamcastAmbientHandle, desired->volume);
            }
            if (desired->musicScale)
                m_ambientMusicDuck =
                    MusicDuck{LegendsDreamcastAmbient::kMusicDuckHold, *desired->musicScale};
        }
    }
    // The N64 listener is the camera target, not its elevated eye. Each IPR
    // ambient record has its own radius; native macros own their loop points.
    const auto ambience =
        m_ambient.update(cameraAttention().value_or(listener), m_destination.has_value());
    std::erase_if(m_ambientSounds, [&](const MotionSound& sound) {
        const auto desired =
            std::ranges::find(ambience, sound.id, &LegendsAmbientVoice::nativeIndex);
        // A finite native macro stays admitted until leaving its zone. Restarting
        // every ended handle would turn one-shot ambience into a fabricated loop.
        if (desired == ambience.end()) {
            output.stop(sound.handle);
            return true;
        }
        return false;
    });
    for (const auto& desired : ambience) {
        const auto active =
            std::ranges::find(m_ambientSounds, desired.nativeIndex, &MotionSound::id);
        if (active == m_ambientSounds.end()) {
            const auto handle = m_sounds.play(output, desired.token, desired.volume);
            if (handle != kNoSound) {
                m_ambientSounds.push_back({desired.nativeIndex, handle});
            }
        } else {
            output.setVolume(active->handle, desired.volume);
        }
    }
    const auto loops = m_world.adapter()->soundLoops();
    const auto audible = [&](const Vec3& position, LegendsSoundRoute route) {
        if (route == LegendsSoundRoute::Global) {
            return 1.0f;
        }
        return std::clamp((70.0f - glm::distance(listener, position)) / 50.0f, 0.0f, 1.0f);
    };
    std::erase_if(m_motionSounds, [&](const MotionSound& sound) {
        const auto desired = std::ranges::find(loops, sound.id, &LegendsSoundLoop::id);
        const bool playing = output.isPlaying(sound.handle);
        if (!playing || desired == loops.end() || audible(desired->position, desired->route) <= 0) {
            if (playing) {
                output.stop(sound.handle);
            }
            return true;
        }
        return false;
    });
    for (const auto& desired : loops) {
        const f32 volume = audible(desired.position, desired.route);
        if (volume <= 0) {
            continue;
        }
        auto active = std::ranges::find(m_motionSounds, desired.id, &MotionSound::id);
        if (active == m_motionSounds.end()) {
            const auto handle = playNativeSound(output, desired.token, volume, true);
            if (handle != kNoSound) {
                m_motionSounds.push_back({desired.id, handle});
            }
        } else {
            output.setVolume(active->handle, volume);
        }
    }
    std::erase_if(m_soundHandles, [&](SoundHandle handle) { return !output.isPlaying(handle); });
    for (const u32 token : std::exchange(m_opponentSounds, {})) {
        const auto handle = playNativeSound(output, token);
        if (handle != kNoSound) {
            m_soundHandles.push_back(handle);
        }
    }
    for (const auto& cue : m_world.adapter()->takeSounds()) {
        // Keep authored macro gain, envelopes and pan, then apply the shared
        // N64 mix calibration before the user's Effects slider.
        const f32 volume = audible(cue.position, cue.route);
        if (volume > 0) {
            const auto handle = playNativeSound(output, cue.token, volume, false);
            if (handle != kNoSound) {
                m_soundHandles.push_back(handle);
            }
        }
    }
}

SoundHandle LegendsLevel::playNativeSound(SoundPlayer& output, u32 token, f32 volume, bool loop) {
    return platform() == LegendsPlatform::Dreamcast
               ? m_dreamcastSounds.play(output, token, volume, loop)
               : m_sounds.play(output, token, volume, loop);
}

void LegendsLevel::continueSpeechFrom(LegendsLevel& previous) {
    if (this != &previous && platform() == LegendsPlatform::Dreamcast &&
        previous.platform() == LegendsPlatform::Dreamcast && previous.m_soundOutput &&
        (!m_soundOutput || m_soundOutput == previous.m_soundOutput) &&
        m_dreamcastSpeech.continueFrom(*previous.m_soundOutput, previous.m_dreamcastSpeech)) {
        m_soundOutput = previous.m_soundOutput;
        m_entrySpeechAwaitQueue = true;
        return;
    }
    if (this == &previous || platform() != LegendsPlatform::N64 ||
        previous.platform() != LegendsPlatform::N64 || !m_speech.opened() ||
        !previous.m_speech.opened() || !previous.m_destination || !previous.m_exitSpeechPlayed ||
        !previous.m_soundOutput) {
        return;
    }
    if (m_soundOutput && m_soundOutput != previous.m_soundOutput) {
        return;
    }
    // Both scenes use the app's long-lived SoundPlayer. Move its narration
    // owner, not the stream cursor or the already-started phrase. Explicitly
    // empty the source: default moves alone would leave its scalar handle live.
    m_speech.stop(*previous.m_soundOutput);
    m_speech = std::exchange(previous.m_speech, {});
    m_soundOutput = previous.m_soundOutput;
    m_entrySpeechAwaitQueue = true;
}

void LegendsLevel::servicePreservedSpeech(f32 seconds) {
    if (m_soundOutput)
        m_speech.update(*m_soundOutput, seconds);
    // Dreamcast finite phrases are already queued in the long-lived SoundPlayer.
    // Do not run level clocks or restart fixture loops behind results/shop menus.
}

void LegendsLevel::pauseSounds() {
    if (m_soundOutput) {
        m_soundOutput->stop(m_deathDrainSound);
        m_speech.update(*m_soundOutput, 0, true);
        for (const auto& sound : m_motionSounds) {
            m_soundOutput->stop(sound.handle);
        }
    }
    m_deathDrainSound = kNoSound;
    m_motionSounds.clear();
}

void LegendsLevel::stopSounds() {
    m_dreamcastVisit.stopSounds();
    pauseSounds();
    if (m_soundOutput) {
        m_speech.stop(*m_soundOutput);
        m_dreamcastSpeech.stop(*m_soundOutput);
        m_soundOutput->stop(m_dreamcastAmbientHandle);
        for (const auto& sound : m_ambientSounds) {
            m_soundOutput->stop(sound.handle);
        }
        for (const auto handle : m_soundHandles) {
            m_soundOutput->stop(handle);
        }
    }
    m_soundHandles.clear();
    m_opponentSounds.clear();
    m_ambientSounds.clear();
    m_dreamcastAmbientHandle = kNoSound;
    m_dreamcastAmbientIndex.reset();
    m_ambientMusicDuck.reset();
    m_soundOutput = nullptr;
    m_deathDrainContact = false;
}

} // namespace gdl::game
