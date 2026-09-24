#include "game/world/PlayerArsenal.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/world/TargetAssist.h"
namespace gdl::game {
namespace {
struct PotionLook {
    std::string_view bottle;
    std::string_view burst;
    std::string_view sound;
};
constexpr std::array<PotionLook, 5> kPotions{{{"POT_RED_TW", "MP_FIRE", "S_POTION2"},
                                              {"POT_RED_TW", "MP_FIRE", "S_POTION2"},
                                              {"POT_BLU_TW", "MP_ELEC", "S_POTION1"},
                                              {"POT_YEL_TW", "MP_LIGHT", "S_POTION3"},
                                              {"POT_GRE_TW", "MP_ACID", "S_POTION4"}}};
constexpr f32 kPotionToss = 5.0f;       ///< how hard a potion is thrown
constexpr f32 kThrownShare = 0.75f;     ///< of that power a thrown potion keeps
constexpr f32 kPotionLoft = 0.707f;     ///< as much up as forwards
constexpr f32 kPotionHandHeight = 4.0f; ///< over the feet, where it leaves
constexpr f32 kPotionHandReach = 2.0f;  ///< and ahead of them

const PotionLook& potionLook(s32 kind) {
    return kPotions[kind >= 0 && static_cast<usize>(kind) < kPotions.size()
                        ? static_cast<usize>(kind)
                        : 0];
}

} // namespace
void PlayerArsenal::bind(const Resources& resources) {
    clear();
    m_resources.emplace(resources);
    loadPotionModels();
}
void PlayerArsenal::clear() {
    m_missiles.clear();
    for (TreeModel& model : m_potionModels) {
        model.clear();
    }
    m_resources.reset();
}
void PlayerArsenal::launchWeapon(const PlayerActor& actor, PlayerFigure* body,
                                 const Vec3& direction, f32 scale, bool spreads,
                                 std::optional<Vec3> target) {
    if (!m_resources.has_value() || body == nullptr) {
        return;
    }
    PlayerFigure& figure = *body;
    Vec3 facing = direction;
    if (target.has_value()) {
        const Vec3 offset = *target - actor.position();
        const f32 distance = std::hypot(offset.x, offset.z);
        if (distance > 1e-5f) {
            facing = Vec3{offset.x / distance, 0, offset.z / distance};
        }
    }
    const CharacterSave& save = actor.save();
    const ClassStats* stats = m_resources->classes.stats(save.character);
    s32 stat = 0;
    Vec3 hand{0.0f, 0.0f, 0.0f};
    if (stats != nullptr) {
        const StatBlock block =
            displayStats(*stats, experienceLevel(save.experience()), save.progress());
        stat = MissileSpec::byMagic(save.character) ? block.magic() : block.strength();
        hand = stats->weaponOffset;
    }
    const Vec3 side{facing.z, 0.0f, -facing.x};
    MissileLaunch launch;
    launch.owner = actor.player();
    launch.scale = scale;
    launch.direction = facing;
    launch.position = actor.followPoint() + side * hand.x + Vec3{0.0f, hand.y, 0.0f} +
                      facing * (hand.z + PlayerMissiles::kMuzzle);
    launch.speed = PlayerMissiles::speedFor(stat);
    launch.damage = PlayerMissiles::damageFor(stat) * scale;
    launch.reach = PlayerMissiles::reachFor(figure.animator().attackSeconds());
    launch.spec = &MissileSpec::of(save.character);
    launch.model = &figure.missile();
    // An obstructed muzzle still produces a world impact, without a flying weapon.
    const f32 radius = launch.spec->radius;
    const Vec3 clear = m_resources->collision.resolveWalls(launch.position, radius,
                                                           launch.position.y - radius * 0.5f,
                                                           launch.position.y + radius * 0.5f);
    if (glm::distance(clear, launch.position) > 1e-4f) {
        presentImpact({.position = clear, .owner = launch.owner});
        return;
    }
    // A three or five way shot worn spreads the throw, fifteen degrees apart.
    const s32 shots = spreads ? PowerupEffects::of(save.progress().inventory).shots() : 1;
    if (shots == 5) {
        launch.wallSound = MissileWallSound::FiveWay;
    } else if (shots == 3) {
        launch.wallSound = MissileWallSound::ThreeWay;
    }
    for (const Vec3& way : PlayerMissiles::spread(facing, shots)) {
        launch.direction = way;
        if (target.has_value()) {
            const Vec3 aim =
                TargetAssist::velocity(launch.position, *target, launch.speed, launch.spec->weight);
            // Preserve spread instead of converging every shot on one point.
            const f32 turn = std::atan2(way.x, way.z) - std::atan2(facing.x, facing.z);
            launch.velocity = Vec3{aim.x * std::cos(turn) + aim.z * std::sin(turn), aim.y,
                                   aim.z * std::cos(turn) - aim.x * std::sin(turn)};
        }
        m_missiles.launch(launch);
        launch.wallSound = MissileWallSound::Silent;
    }
    if (const auto sound = figure.throwSound();
        m_resources->sounds != nullptr && sound.has_value()) {
        try {
            m_resources->sounds->play(figure.voice().sequence(*sound), 1.0f,
                                      SoundCategory::Effects);
        } catch (const std::exception& e) {
            log::warn("Tower: throw sound: {}", e.what());
        }
    }
}

void PlayerArsenal::loadPotionModels() {
    if (!m_resources.has_value() || !m_resources->weapons.loaded()) {
        return;
    }
    for (usize kind = 0; kind < m_potionModels.size(); ++kind) {
        if (const auto tree = m_resources->weapons.trees.find(kPotions[kind].bottle);
            tree.has_value()) {
            m_potionModels[kind].bind(m_resources->weapons.trees.tree(*tree),
                                      m_resources->weapons.models, m_resources->weapons.textures,
                                      m_resources->device);
        }
    }
}

void PlayerArsenal::burstPotion(s32 kind, const Vec3& position, f32 power) {
    if (!m_resources.has_value()) {
        return;
    }
    const PotionLook& look = potionLook(kind);
    if (m_resources->weapons.loaded()) {
        m_resources->effects.start(m_resources->device, m_resources->weapons, look.burst, position,
                                   std::min(kBurstPerPower * power, 1.0f));
    }
    m_resources->audio.playNamed(look.sound);
}

void PlayerArsenal::presentImpact(const MissileImpact& impact) {
    if (!m_resources) {
        return;
    }
    if (impact.potion != 0) {
        burstPotion(impact.potion, impact.position, impact.potency);
        return;
    }
    if (impact.target >= 0) {
        return; // Target owners supply their own material/body feedback.
    }
    if (m_resources->weapons.loaded()) {
        EffectTrees::Setting setting;
        setting.unlit = true;
        setting.depthWrite = false;
        if (impact.effect == "SPARKS") {
            setting.tint.a = 96;
        }
        m_resources->effects.startSet(m_resources->device, m_resources->weapons, impact.effect,
                                      impact.position, setting);
    }
    switch (impact.wallSound) {
    case MissileWallSound::Level: m_resources->audio.playNamed(m_resources->wallHitSound); break;
    case MissileWallSound::ThreeWay: m_resources->audio.playNamed("S_3WAYAXE"); break;
    case MissileWallSound::FiveWay: m_resources->audio.playNamed("S_5WAYAXE"); break;
    case MissileWallSound::Silent: break;
    }
}

f32 PlayerArsenal::magicPowerOf(const PlayerActor& actor) const {
    const CharacterSave& save = actor.save();
    const ClassStats* stats =
        m_resources.has_value() ? m_resources->classes.stats(save.character) : nullptr;
    const s32 magic =
        stats != nullptr
            ? displayStats(*stats, experienceLevel(save.experience()), save.progress()).magic()
            : 0;
    return PowerupEffects::of(save.progress().inventory).magicPower(magic);
}

void PlayerArsenal::usePotion(PlayerActor& actor) {
    if (!m_resources.has_value()) {
        return;
    }
    if (const s32 kind = actor.save().progress().inventory.takePotion(); kind != 0) {
        burstPotion(kind, actor.position(), magicPowerOf(actor));
    }
}

void PlayerArsenal::throwPotion(PlayerActor& actor) {
    if (!m_resources.has_value()) {
        return;
    }
    const s32 kind = actor.save().progress().inventory.takePotion();
    if (kind == 0) {
        return;
    }
    const Vec3 facing = actor.facing();
    MissileLaunch launch;
    launch.owner = actor.player();
    launch.direction = facing;
    launch.position =
        actor.position() + facing * kPotionHandReach + Vec3{0.0f, kPotionHandHeight, 0.0f};
    launch.velocity =
        Vec3{facing.x * kPotionLoft, kPotionLoft, facing.z * kPotionLoft} * kPotionToss;
    launch.potion = kind;
    launch.potency = kThrownShare * magicPowerOf(actor);
    launch.spec = &MissileSpec::potion();
    launch.model = &m_potionModels[static_cast<usize>(
        std::clamp(kind, 0, static_cast<s32>(m_potionModels.size()) - 1))];
    m_missiles.launch(launch);
}
} // namespace gdl::game
