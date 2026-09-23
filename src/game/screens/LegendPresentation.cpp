#include "game/screens/LegendPresentation.h"

#include <algorithm>
#include <string>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"

namespace gdl::game {

LegendPresentation::LegendPresentation(EffectTrees& effects, Assets assets, Audio audio)
    : m_effects(effects), m_assets(assets), m_audio(std::move(audio)) {}

LegendPresentation::~LegendPresentation() {
    clear();
}

void LegendPresentation::clear() {
    stopLoop();
    for (const u32 id : m_ownedEffects) {
        m_effects.stop(id);
    }
    m_ownedEffects.clear();
    m_player = -1;
    m_kind = -1;
    m_realm = 'A';
    m_held = 0;
    m_flying = 0;
    m_gestureOwed = false;
    m_released = false;
    m_flightLeft = 0.0f;
    m_frozenTexture = nullptr;
}

u32 LegendPresentation::start(ItemArchive& archive, std::string_view tree, const Vec3& position,
                              const EffectTrees::Setting& setting) {
    const u32 id = m_effects.startSet(m_assets.device, archive, tree, position, setting);
    if (id != 0) {
        m_ownedEffects.push_back(id);
    }
    return id;
}

void LegendPresentation::show(LegendCue cue, s32 player, s32 realm, s32 kind,
                              const std::optional<Bearer>& bearer) {
    if (cue == LegendCue::Brandished) {
        clear();
        m_kind = kind;
        m_realm = static_cast<char>('A' + std::clamp(realm - 1, 0, 25));
        if (bearer.has_value() && bearer->player == player) {
            m_player = player;
            brandish(*bearer);
        }
        playSound(LegendShow::Sound::PickedUp);
        return;
    }
    if (player != m_player) {
        return;
    }
    switch (cue) {
    case LegendCue::Thrown:
        if (m_player >= 0 && !m_released) {
            m_gestureOwed = true;
            playSound(LegendShow::Sound::Thrown);
        }
        break;
    case LegendCue::WornOff:
        stopLoop();
        if (LegendShow::flightOf(m_kind) == LegendShow::Flight::AtBoss) {
            playSound(LegendShow::Sound::Landed);
        }
        playSound(LegendShow::Sound::WornOff);
        break;
    case LegendCue::Brandished:
    case LegendCue::Roared: break;
    }
}

void LegendPresentation::brandish(const Bearer& bearer) {
    if (m_assets.items.loaded() && m_assets.items.trees.find(LegendShow::kHeldTree).has_value()) {
        EffectTrees::Setting setting;
        setting.seconds = LegendShow::kHeldSeconds;
        setting.unlit = true;
        setting.depthWrite = false;
        m_held = start(m_assets.items, LegendShow::kHeldTree, bearer.holdPoint, setting);
    }
    if (!m_assets.weapons.loaded()) {
        return;
    }
    EffectTrees::Setting charge;
    charge.unlit = true;
    charge.depthWrite = false;
    charge.tint = LegendShow::chargeTint(bearer.color);
    start(m_assets.weapons, LegendShow::kAuraTree, bearer.position, charge);
    charge.tint = Color::white();
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
    std::erase_if(m_ownedEffects, [this](u32 id) { return !m_effects.playing(id); });
    if (!bearer.has_value() || m_player < 0 || bearer->player != m_player) {
        return result;
    }
    if (m_gestureOwed) {
        if (!bearer->canGesture) {
            m_gestureOwed = false;
            release(*bearer, target);
        } else if (bearer->casting) {
            m_gestureOwed = false;
        } else {
            result.gesture = LegendShow::gestureOf(m_kind);
        }
    }
    if (bearer->released) {
        release(*bearer, target);
    }
    if (m_held != 0) {
        if (m_effects.playing(m_held)) {
            m_effects.moveTo(m_held, bearer->holdPoint);
        } else {
            m_held = 0;
        }
    }
    if (m_flying != 0 && !m_effects.playing(m_flying)) {
        m_flying = 0;
    }
    if (m_flying != 0 && LegendShow::flightOf(m_kind) == LegendShow::Flight::Flies) {
        m_flightLeft -= seconds;
        if (m_flightLeft <= 0.0f) {
            land(target);
            result.landed = true;
        }
    } else if (m_flying != 0 && LegendShow::flightOf(m_kind) == LegendShow::Flight::WithBearer) {
        m_effects.moveTo(m_flying, bearer->position + bearer->facing * LegendShow::kAhead);
    }
    return result;
}

void LegendPresentation::release(const Bearer& bearer, const std::optional<Target>& target) {
    if (m_released) {
        return;
    }
    m_released = true;
    m_effects.stop(m_held);
    m_held = 0;
    if (!m_assets.items.loaded() || !target.has_value()) {
        return;
    }
    EffectTrees::Setting setting;
    setting.unlit = true;
    setting.depthWrite = false;
    switch (LegendShow::flightOf(m_kind)) {
    case LegendShow::Flight::Flies: {
        const Vec3 from = bearer.position + bearer.facing + Vec3{0.0f, LegendShow::kLift, 0.0f};
        const Vec3 to = target->position + Vec3{0.0f, target->height * 0.5f, 0.0f};
        const f32 distance = glm::length(to - from);
        m_flightLeft = std::min(distance / LegendShow::kSpeed, LegendShow::kFlightSeconds);
        if (distance > 0.0f) {
            setting.velocity = (to - from) * (LegendShow::kSpeed / distance);
        }
        setting.seconds = LegendShow::kFlightSeconds;
        m_flying = start(m_assets.items, LegendShow::kProjectileTree, from, setting);
        break;
    }
    case LegendShow::Flight::AtBoss:
        setting.seconds = LegendShow::burstSecondsOf(m_kind);
        setting.then = std::string(LegendShow::burstTreeOf(m_kind));
        m_flying = start(m_assets.items, LegendShow::restingTreeOf(m_kind),
                         target->position + LegendShow::bossOffsetOf(m_kind), setting);
        break;
    case LegendShow::Flight::WithBearer:
        setting.seconds = LegendShow::burstSecondsOf(m_kind);
        setting.then = std::string(LegendShow::burstTreeOf(m_kind));
        m_flying = start(m_assets.items, LegendShow::kProjectileTree,
                         bearer.position + bearer.facing * LegendShow::kAhead, setting);
        break;
    }
    const ParticleDescriptor trail = LegendShow::trailOf(m_kind);
    if (m_flying != 0 && !trail.texture.empty()) {
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
            m_effects.attachTrail(m_flying, trail, textures->texture(m_assets.device, *slot));
        } else {
            log::warn("Legend item: missing trail texture {}", trail.texture);
        }
    }
    playSound(LegendShow::Sound::Flying, true);
}

void LegendPresentation::land(const std::optional<Target>& target) {
    if (m_kind == 34) {
        TextureSet& textures = m_assets.items.textures;
        if (const auto slot = textures.find("SEETHROUGH"); slot.has_value()) {
            m_frozenTexture = &textures.texture(m_assets.device, *slot);
        }
    }
    m_effects.stop(m_flying);
    m_flying = 0;
    stopLoop();
    if (target.has_value() && m_assets.items.loaded() &&
        m_assets.items.trees.find(LegendShow::kBurstTree).has_value()) {
        start(m_assets.items, LegendShow::kBurstTree,
              target->position + Vec3{0.0f, target->height * 0.5f, 0.0f}, {});
    }
    playSound(LegendShow::Sound::Landed);
}

void LegendPresentation::playSound(LegendShow::Sound sound, bool looping) {
    if (!m_audio.play) {
        return;
    }
    for (const std::string& name : LegendShow::soundNamesOf(sound, m_realm)) {
        if (const SoundHandle handle = m_audio.play(name); handle != kNoSound) {
            if (looping) {
                stopLoop();
                m_loop = handle;
            }
            return;
        }
    }
}

void LegendPresentation::stopLoop() {
    if (m_loop != kNoSound && m_audio.stop) {
        m_audio.stop(m_loop);
    }
    m_loop = kNoSound;
}

} // namespace gdl::game
