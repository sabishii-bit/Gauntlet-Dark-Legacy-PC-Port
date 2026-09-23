#include "game/world/PlayerArsenal.h"

#include <algorithm>
#include <cstddef>
#include <exception>

#include "engine/core/Log.h"

#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
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
constexpr float kPotionToss = 5.0f;       ///< how hard a potion is thrown
constexpr float kThrownShare = 0.75f;     ///< of that power a thrown potion keeps
constexpr float kPotionLoft = 0.707f;     ///< as much up as forwards
constexpr float kPotionHandHeight = 4.0f; ///< over the feet, where it leaves
constexpr float kPotionHandReach = 2.0f;  ///< and ahead of them

const PotionLook& potionLook(int kind) {
    return kPotions[kind >= 0 && static_cast<std::size_t>(kind) < kPotions.size()
                        ? static_cast<std::size_t>(kind)
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
                                 const Vec3& direction, float scale, bool spreads) {
    if (!m_resources.has_value() || body == nullptr) {
        return;
    }
    PlayerFigure& figure = *body;
    const Vec3 facing = direction;
    const CharacterSave& save = actor.save();
    const ClassStats* stats = m_resources->classes.stats(save.character);
    int stat = 0;
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
    // Thrown into a wall at arm's length, nothing flies.
    const float radius = launch.spec->radius;
    const Vec3 clear = m_resources->collision.resolveWalls(launch.position, radius,
                                                           launch.position.y - radius * 0.5f,
                                                           launch.position.y + radius * 0.5f);
    if (glm::distance(clear, launch.position) > 1e-4f) {
        return;
    }
    // A three or five way shot worn spreads the throw, fifteen degrees apart.
    const int shots = spreads ? PowerupEffects::of(save.progress().inventory).shots() : 1;
    for (const Vec3& way : PlayerMissiles::spread(facing, shots)) {
        launch.direction = way;
        m_missiles.launch(launch);
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
    for (std::size_t kind = 0; kind < m_potionModels.size(); ++kind) {
        if (const auto tree = m_resources->weapons.trees.find(kPotions[kind].bottle);
            tree.has_value()) {
            m_potionModels[kind].bind(m_resources->weapons.trees.tree(*tree),
                                      m_resources->weapons.models, m_resources->weapons.textures,
                                      m_resources->device);
        }
    }
}

void PlayerArsenal::burstPotion(int kind, const Vec3& position, float power) {
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

float PlayerArsenal::magicPowerOf(const PlayerActor& actor) const {
    const CharacterSave& save = actor.save();
    const ClassStats* stats =
        m_resources.has_value() ? m_resources->classes.stats(save.character) : nullptr;
    const int magic =
        stats != nullptr
            ? displayStats(*stats, experienceLevel(save.experience()), save.progress()).magic()
            : 0;
    return PowerupEffects::of(save.progress().inventory).magicPower(magic);
}

void PlayerArsenal::usePotion(PlayerActor& actor) {
    if (!m_resources.has_value()) {
        return;
    }
    if (const int kind = actor.save().progress().inventory.takePotion(); kind != 0) {
        burstPotion(kind, actor.position(), magicPowerOf(actor));
    }
}

void PlayerArsenal::throwPotion(PlayerActor& actor) {
    if (!m_resources.has_value()) {
        return;
    }
    const int kind = actor.save().progress().inventory.takePotion();
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
    launch.model = &m_potionModels[static_cast<std::size_t>(
        std::clamp(kind, 0, static_cast<int>(m_potionModels.size()) - 1))];
    m_missiles.launch(launch);
}
} // namespace gdl::game
