#include "game/bosses/LegendPresentation.h"

#include <algorithm>
#include <string>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"
#include "engine/world/AnimationPlayer.h"

#include "game/effects/DynamicLights.h"

namespace gdl::game {
void LegendPresentation::appendState(NativeStateDigest& digest, const Simulation& state) {
    const auto& [ownedEffects, player, kind, realm, held, flying, attached, gestureOwed, released,
                 brandished, flightLeft, flightPosition, frozenTexture, flightCue, flightSound] =
        state;
    digest.string("LegendPresentation.native.1");
    digest.values(ownedEffects, player, kind, realm, held, flying, attached, gestureOwed, released,
                  brandished, flightLeft, flightPosition);
    digest.resource(frozenTexture);
    digest.values(flightCue, flightSound);
}

bool LegendPresentation::appendNativeDigest(NativeStateDigest& digest) const {
    appendState(digest, m_sim);
    return digest.good();
}

bool LegendPresentation::appendLocalDigest(NativeStateDigest& digest,
                                           const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, owner, binding] = saved;
    appendState(digest, state);
    return digest.good();
}

void LegendPresentation::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    saved.m_state = m_sim;
}

bool LegendPresentation::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_localBinding;
}

bool LegendPresentation::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
}

LegendPresentation::LegendPresentation(EffectTrees& effects, Assets assets, Audio audio)
    : m_effects(effects), m_assets(assets), m_audio(std::move(audio)) {}

LegendPresentation::~LegendPresentation() {
    clear();
}

void LegendPresentation::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    reset();
}

void LegendPresentation::reset() {
    stopLoop();
    for (const u32 id : m_sim.ownedEffects) {
        m_effects.stop(id);
    }
    m_sim.ownedEffects.clear();
    m_sim.player = -1;
    m_sim.kind = -1;
    m_sim.realm = 'A';
    m_sim.held = 0;
    m_sim.flying = 0;
    m_sim.attached = 0;
    m_sim.gestureOwed = false;
    m_sim.released = false;
    m_sim.brandished = false;
    m_sim.flightLeft = 0.0f;
    m_sim.flightPosition = Vec3{0};
    m_sim.frozenTexture = nullptr;
}

u32 LegendPresentation::start(ItemArchive& archive, std::string_view tree, const Vec3& position,
                              const EffectTrees::Setting& setting) {
    const u32 id = m_effects.startSet(m_assets.device, archive, tree, position, setting);
    if (id != 0) {
        m_sim.ownedEffects.push_back(id);
    }
    return id;
}

void LegendPresentation::carry(s32 player, s32 realm, s32 kind,
                               const std::optional<Bearer>& bearer) {
    if (!bearer || bearer->player != player) {
        return;
    }
    if (m_sim.player != player || m_sim.kind != kind) {
        reset();
        m_sim.player = player;
        m_sim.kind = kind;
        m_sim.realm = static_cast<char>('A' + std::clamp(realm - 1, 0, 25));
    }
    if (!m_sim.released) {
        hold(*bearer);
    }
}

LegendShow::Flight LegendPresentation::flight() const {
    return m_assets.profile && m_assets.profile->flight ? *m_assets.profile->flight
                                                        : LegendShow::flightOf(m_sim.kind);
}

std::string_view LegendPresentation::heldTree() const {
    return m_assets.profile ? m_assets.profile->held : LegendShow::kHeldTree;
}

std::string_view LegendPresentation::projectileTree() const {
    return m_assets.profile ? m_assets.profile->projectile : LegendShow::kProjectileTree;
}

std::string_view LegendPresentation::burstTree() const {
    return m_assets.profile ? m_assets.profile->burst : LegendShow::kBurstTree;
}

s32 LegendPresentation::occupiedHand() const {
    return LegendShow::heldInHand(m_sim.kind) && m_sim.held != 0 && m_effects.playing(m_sim.held)
               ? m_sim.player
               : -1;
}

void LegendPresentation::show(LegendCue cue, s32 player, s32 realm, s32 kind,
                              const std::optional<Bearer>& bearer) {
    if (cue == LegendCue::Brandished) {
        carry(player, realm, kind, bearer);
        if (!m_sim.brandished && bearer && m_sim.player == player) {
            m_sim.brandished = true;
            brandish(*bearer);
            playSound(LegendShow::Sound::PickedUp);
        }
        return;
    }
    if (player != m_sim.player) {
        return;
    }
    switch (cue) {
    case LegendCue::Thrown:
        if (m_sim.player >= 0 && !m_sim.released) {
            m_sim.gestureOwed = true;
            playSound(LegendShow::Sound::Thrown);
        }
        break;
    case LegendCue::WornOff:
        // world_update restores PBOSSEYEBALL and clears blindness silently;
        // it does not take the other items' kill/ALSTP/PDN sound branch.
        if (LegendShow::restoresSight(m_sim.kind)) {
            break;
        }
        stopLoop();
        if (flight() == LegendShow::Flight::AtBoss) {
            playSound(LegendShow::Sound::Landed);
        }
        playSound(LegendShow::Sound::WornOff);
        break;
    case LegendCue::Brandished:
    case LegendCue::Roared: break;
    }
}

void LegendPresentation::hold(const Bearer& bearer) {
    if (m_sim.held == 0 && m_assets.items.loaded() &&
        m_assets.items.trees.find(heldTree()).has_value()) {
        EffectTrees::Setting setting;
        setting.seconds = LegendShow::kHeldSeconds;
        setting.unlit = true;
        setting.depthWrite = false;
        m_sim.held = start(m_assets.items, heldTree(), bearer.holdPoint, setting);
    }
    if (m_sim.held != 0) {
        m_effects.placeAt(m_sim.held,
                          bearer.holdTransform.value_or(glm::translate(Mat4{1}, bearer.holdPoint)));
    }
}

void LegendPresentation::brandish(const Bearer& bearer) {
    if (m_assets.profile && !m_assets.profile->charge) {
        return;
    }
    if (!m_assets.weapons.loaded()) {
        return;
    }
    EffectTrees::Setting charge;
    charge.unlit = true;
    charge.depthWrite = false;
    charge.tint = LegendShow::chargeTint(bearer.color);
    charge.light =
        EffectTrees::Light{DynamicLights::ofCostume(bearer.color), DynamicLights::kChargeRadius};
    start(m_assets.weapons, LegendShow::kAuraTree, bearer.position, charge);
    charge.tint = Color::white();
    charge.light.reset();
    charge.playbackRate = LegendShow::kBurstPlaybackRate;
    if (const auto burst = m_assets.weapons.trees.find(LegendShow::chargeTree(bearer.color));
        burst.has_value()) {
        const TreeInfo& tree = m_assets.weapons.trees.tree(*burst);
        if (!tree.sequences.empty()) {
            const TreeSequenceInfo& sequence = tree.sequences.front();
            charge.seconds =
                static_cast<f32>(sequence.frames * sequence.frameRate) * AnimationPlayer::kRateUnit;
            charge.loop = false;
        }
        start(m_assets.weapons, tree.name, bearer.position, charge);
    }
}

LegendPresentation::Update LegendPresentation::update(f32 seconds,
                                                      const std::optional<Bearer>& bearer,
                                                      const std::optional<Target>& target) {
    Update result;
    std::erase_if(m_sim.ownedEffects, [this](u32 id) { return !m_effects.playing(id); });
    followTarget(target);
    if (m_sim.player < 0) {
        return result;
    }
    const bool hasBearer = bearer.has_value() && bearer->player == m_sim.player;
    const bool wasReleased = m_sim.released;
    if (m_sim.gestureOwed && hasBearer) {
        if (!bearer->canGesture) {
            m_sim.gestureOwed = false;
            release(*bearer, target);
        } else if (bearer->casting) {
            m_sim.gestureOwed = false;
        } else {
            result.gesture = LegendShow::gestureOf(m_sim.kind);
        }
    }
    if (hasBearer && bearer->released) {
        release(*bearer, target);
    }
    if (LegendShow::landsOnRelease(m_sim.kind) && !wasReleased && m_sim.released) {
        result.landed = true;
    }
    if (m_sim.held != 0 && hasBearer) {
        if (m_effects.playing(m_sim.held)) {
            hold(*bearer);
        } else {
            m_sim.held = 0;
        }
    } else if (m_sim.held != 0) {
        m_effects.stop(m_sim.held);
        m_sim.held = 0;
    }
    if (m_sim.flying != 0 && !m_effects.playing(m_sim.flying)) {
        m_sim.flying = 0;
        stopLoop();
    }
    if (m_sim.flying != 0 && flight() == LegendShow::Flight::Flies) {
        m_sim.flightLeft -= seconds;
        if (LegendShow::homes(m_sim.kind)) {
            // ProcessEffects' 0x40000000 branch recomputes velocity toward
            // the target node each update; a launch-time travel timer misses
            // the moving eye during PBOSS's roar.
            if (target && seconds > 0) {
                const Vec3 destination = target->position + Vec3{0, target->height * 0.5f, 0};
                const Vec3 delta = destination - m_sim.flightPosition;
                const f32 distance = glm::length(delta);
                const f32 step = LegendShow::kSpeed * seconds;
                const Vec3 next = distance <= step
                                      ? destination
                                      : m_sim.flightPosition + delta * (step / distance);
                // PlayerMotion supplies colrad=2 (GC 0x80347b98). ProcessEffects
                // sweeps against CritterMoveNodeCol; reaching the eye's exact
                // moving origin is not required to hit and replace its mesh.
                constexpr f32 kJavelinRadius = 2;
                const bool contact =
                    target->touches ? target->touches(m_sim.flightPosition, next, kJavelinRadius)
                                    : distance <= step;
                if (contact) {
                    land(target);
                    result.landed = true;
                } else {
                    m_sim.flightPosition = next;
                    m_effects.placeAt(m_sim.flying, glm::translate(Mat4{1}, m_sim.flightPosition),
                                      distance > 0 ? std::optional{delta / distance}
                                                   : std::nullopt);
                }
            }
            if (m_sim.flightLeft <= 0 && m_sim.flying != 0) {
                m_effects.stop(m_sim.flying);
                m_sim.flying = 0;
                stopLoop(); // A flight that never reaches its target is not a hit.
            }
        } else if (m_sim.flightLeft <= 0.0f) {
            land(target);
            result.landed = true;
        }
    } else if (m_sim.flying != 0 && hasBearer && flight() == LegendShow::Flight::WithBearer) {
        const Mat4 root =
            portable::rotate(glm::translate(Mat4{1}, bearer->position),
                             portable::atan2(bearer->facing.x, bearer->facing.z), Vec3{0, 1, 0});
        m_effects.placeAt(m_sim.flying, glm::translate(root, Vec3{0, 0, LegendShow::kAhead}));
    }
    return result;
}

void LegendPresentation::release(const Bearer& bearer, const std::optional<Target>& target) {
    if (m_sim.released) {
        return;
    }
    m_sim.released = true;
    m_effects.stop(m_sim.held);
    m_sim.held = 0;
    if (!m_assets.items.loaded() || !target.has_value()) {
        return;
    }
    EffectTrees::Setting setting;
    setting.unlit = true;
    setting.depthWrite = false;
    switch (flight()) {
    case LegendShow::Flight::Flies: {
        const Vec3 from = bearer.position + bearer.facing + Vec3{0.0f, LegendShow::kLift, 0.0f};
        const Vec3 to = target->position + Vec3{0.0f, target->height * 0.5f, 0.0f};
        const f32 distance = glm::length(to - from);
        const bool homes = LegendShow::homes(m_sim.kind);
        m_sim.flightPosition = from;
        m_sim.flightLeft =
            homes ? LegendShow::kFlightSeconds
                  : std::min(distance / LegendShow::kSpeed, LegendShow::kFlightSeconds);
        if (distance > 0.0f && !homes) {
            setting.velocity = (to - from) * (LegendShow::kSpeed / distance);
        }
        setting.seconds = LegendShow::kFlightSeconds;
        m_sim.flying = start(m_assets.items, projectileTree(), from, setting);
        break;
    }
    case LegendShow::Flight::AtBoss:
        setting.seconds = LegendShow::burstSecondsOf(m_sim.kind);
        setting.then = m_assets.profile ? m_assets.profile->secondBurst
                                        : std::string(LegendShow::burstTreeOf(m_sim.kind));
        m_sim.flying =
            start(m_assets.items,
                  m_assets.profile ? projectileTree() : LegendShow::restingTreeOf(m_sim.kind),
                  target->position + (m_assets.profile ? m_assets.profile->bossOffset
                                                       : LegendShow::bossOffsetOf(m_sim.kind)),
                  setting);
        break;
    case LegendShow::Flight::WithBearer:
        setting.seconds = LegendShow::burstSecondsOf(m_sim.kind);
        setting.then = m_assets.profile ? m_assets.profile->secondBurst
                                        : std::string(LegendShow::burstTreeOf(m_sim.kind));
        m_sim.flying = start(m_assets.items, projectileTree(),
                             bearer.position + bearer.facing * LegendShow::kAhead, setting);
        break;
    }
    const ParticleDescriptor trail = m_assets.profile && !m_assets.profile->trail
                                         ? ParticleDescriptor{}
                                         : LegendShow::trailOf(m_sim.kind);
    if (m_sim.flying != 0 && !trail.texture.empty()) {
        TextureSet* textures = &m_assets.items.textures;
        auto slot = textures->find(trail.texture);
        if (!slot.has_value()) {
            textures = &m_assets.weapons.textures;
            slot = textures->find(trail.texture);
        }
        if (!slot.has_value()) {
            textures = &m_assets.sharedTextures;
            slot = textures->find(trail.texture);
        }
        if (slot.has_value()) {
            m_effects.attachTrail(m_sim.flying, trail, textures->texture(m_assets.device, *slot));
        } else {
            log::warn("Legend item: missing trail texture {}", trail.texture);
        }
    }
    playSound(LegendShow::Sound::Flying, true);
}

void LegendPresentation::land(const std::optional<Target>& target) {
    if (const std::string_view skin = LegendShow::frozenSkinOf(m_sim.kind); !skin.empty()) {
        TextureSet& textures = m_assets.items.textures;
        if (const auto slot = textures.find(skin); slot.has_value()) {
            m_sim.frozenTexture = &textures.texture(m_assets.device, *slot);
        }
    }
    m_effects.stop(m_sim.flying);
    m_sim.flying = 0;
    stopLoop();
    if (target && target->attachment && m_assets.profile &&
        m_assets.profile->impactAttachmentSeconds > 0) {
        EffectTrees::Setting setting;
        setting.seconds = m_assets.profile->impactAttachmentSeconds;
        setting.unlit = true;
        setting.depthWrite = false;
        m_sim.attached = start(m_assets.items, projectileTree(), target->position, setting);
        followTarget(target);
    } else if (target.has_value() && m_assets.items.loaded() &&
               m_assets.items.trees.find(burstTree()).has_value()) {
        const f32 follow = LegendShow::burstFollowSecondsOf(m_sim.kind);
        EffectTrees::Setting setting;
        if (follow > 0.0f) {
            setting.seconds = follow;
        }
        const u32 effect =
            start(m_assets.items, burstTree(),
                  target->position + Vec3{0.0f, target->height * 0.5f, 0.0f}, setting);
        if (follow > 0.0f) {
            m_sim.attached = effect;
            followTarget(target);
        }
    }
    playSound(LegendShow::Sound::Landed);
}

void LegendPresentation::followTarget(const std::optional<Target>& target) {
    if (m_sim.attached == 0) {
        return;
    }
    if (!target.has_value() || !m_effects.playing(m_sim.attached)) {
        m_effects.stop(m_sim.attached);
        m_sim.attached = 0;
        return;
    }
    if (m_assets.profile && m_assets.profile->impactAttachmentSeconds > 0) {
        if (target->attachment) {
            m_effects.placeAt(m_sim.attached, *target->attachment);
        } else {
            m_effects.stop(m_sim.attached);
            m_sim.attached = 0;
        }
        return;
    }
    const Mat4 root = target->root.value_or(glm::translate(Mat4{1.0f}, target->position));
    m_effects.placeAt(m_sim.attached, glm::translate(root, Vec3{0.0f, 6.0f, 0.0f}));
}

void LegendPresentation::playSound(LegendShow::Sound sound, bool looping) {
    if (m_audio.mapped && m_audio.mapped(sound)) {
        return;
    }
    if (!m_audio.play && !m_audio.record) {
        return;
    }
    for (const std::string& name : LegendShow::soundNamesOf(sound, m_sim.realm)) {
        if (m_audio.available && !m_audio.available(name)) {
            continue;
        }
        if (m_audio.record) {
            if (const auto recorded = m_audio.record(name, looping, m_sim.player, m_sim.kind,
                                                     static_cast<u32>(sound) + 1)) {
                if (looping) {
                    m_sim.flightCue = *recorded;
                    m_sim.flightSound = name;
                }
                return;
            }
        }
        if (!m_audio.play) {
            return;
        }
        if (const SoundHandle handle = m_audio.play(name); handle != kNoSound) {
            if (looping) {
                stopLoop();
                m_loop = handle;
            }
            return;
        }
        if (m_audio.available) {
            return; // A known cue's playback failure must not select another bank/name.
        }
    }
}

void LegendPresentation::stopLoop() {
    m_sim.flightCue = {};
    m_sim.flightSound.clear();
    if (m_loop != kNoSound && m_audio.stop) {
        m_audio.stop(m_loop);
    }
    m_loop = kNoSound;
}

void LegendPresentation::gatherAudio() const {
    if (m_sim.flightCue && m_audio.desire) {
        m_audio.desire(m_sim.flightSound, m_sim.player, m_sim.kind, m_sim.flightCue);
    }
}

} // namespace gdl::game
