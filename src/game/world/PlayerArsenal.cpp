#include "game/world/PlayerArsenal.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/combat/DamageTypes.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/world/DynamicLights.h"
#include "game/world/TargetAssist.h"
#include "game/world/WeaponGlow.h"
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
constexpr f32 kPotionCharge = 1.5f;     ///< extra launch speed per held game tick
constexpr f32 kThrownShare = 0.75f;     ///< of that power a thrown potion keeps
constexpr f32 kPotionLoft = 0.707f;     ///< as much up as forwards
constexpr f32 kPotionHandHeight = 4.0f; ///< over the feet, where it leaves
constexpr f32 kPotionHandReach = 2.0f;  ///< and ahead of them
constexpr f32 kPhoenixDamage = 10.0f;
constexpr u32 kPhoenixDamageFlags = 0x11; ///< fire plus knockback

const PotionLook& potionLook(s32 kind) {
    return kPotions[kind >= 0 && static_cast<usize>(kind) < kPotions.size()
                        ? static_cast<usize>(kind)
                        : 0];
}

} // namespace
void PlayerArsenal::bind(const Resources& resources, std::span<TextureSet* const> textureLenders) {
    clear();
    m_resources.emplace(resources);
    m_missiles.bindVisuals(resources.device, textureLenders);
    loadPotionModels();
    if (const auto tree = resources.weapons.trees.find("PHOENIX_FBALL")) {
        m_phoenixShot.bind(resources.weapons.trees.tree(*tree), resources.weapons.models,
                           resources.weapons.textures, resources.device);
    }
    if (const auto tree = resources.weapons.trees.find("SUPERARROW")) {
        m_superShot.bind(resources.weapons.trees.tree(*tree), resources.weapons.models,
                         resources.weapons.textures, resources.device);
    }
    constexpr std::array<std::string_view, 2> kGauntlets{"BOSSG_ACID", "BOSSG_ELEC"};
    for (usize i = 0; i < kGauntlets.size(); ++i) {
        if (const auto tree = resources.weapons.trees.find(kGauntlets[i])) {
            m_gauntlets[i].bind(resources.weapons.trees.tree(*tree), resources.weapons.models,
                                resources.weapons.textures, resources.device);
        }
    }
}
void PlayerArsenal::clear() {
    if (m_resources) {
        for (const auto& cast : m_castEffects) {
            m_resources->effects.stop(cast.effect);
        }
    }
    m_castEffects.clear();
    m_missiles.clear();
    m_superShot.clear();
    m_phoenixShot.clear();
    for (auto& model : m_gauntlets) {
        model.clear();
    }
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
    launch.multiplayer =
        m_resources->multiplayer != nullptr ? *m_resources->multiplayer : MultiplayerMode::Normal;
    launch.owner = actor.player();
    launch.scale = scale;
    launch.direction = facing;
    launch.position = actor.followPoint() + side * hand.x + Vec3{0.0f, hand.y, 0.0f} +
                      facing * (hand.z + PlayerMissiles::kMuzzle);
    launch.speed = PlayerMissiles::speedFor(stat);
    launch.damage = PlayerMissiles::damageFor(stat) * scale;
    launch.flags = PowerupEffects::of(save.progress().inventory).weapon & ~powerup::kSuperShot;
    launch.reach = PlayerMissiles::reachFor(figure.animator().attackSeconds());
    launch.spec = &MissileSpec::of(save.character);
    launch.playerHitGap = PlayerMissiles::hitGap(&m_resources->weapons, launch.spec->impactTree);
    launch.model = &figure.missile();
    launch.archive = figure.missileArchive();
    launch.tree = figure.missileTree();
    // An elemental weapon's throw carries the element's WEAP_TW effect of the costume
    // colour's effects (PlayerStartMissile, combat.c 1030); the wizards and sorceresses
    // throw that effect alone, their weapon unseen.
    const std::string rider =
        WeaponGlow::throwTree(WeaponGlow::elementOf(PowerupEffects::of(save.progress().inventory)));
    if (ItemArchive* effects = rider.empty() ? nullptr : figure.effects();
        effects != nullptr && effects->trees.find(rider).has_value()) {
        launch.riderArchive = effects;
        launch.riderTree = rider;
        if (WeaponGlow::throwsEffectAlone(save.character)) {
            launch.model = nullptr;
            launch.archive = nullptr;
            launch.tree = {};
        }
    }
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
    const u32 weapon = PowerupEffects::of(save.progress().inventory).weapon;
    constexpr std::array<std::string_view, 5> kElementThrows{"", "S_AMULETFIRE", "S_AMULETLIGHTNI",
                                                             "S_AMULETLIGHT", "S_AMULETACID"};
    constexpr u32 kSpecialThrows =
        powerup::kThreeWayShot | powerup::kFiveWayShot | powerup::kSuperShot;
    const auto element = static_cast<usize>(weapon & 0xFU);
    if ((weapon & kSpecialThrows) != 0) {
        m_resources->audio.playNamed("S_SUPERSHOT", LevelSoundscape::kStepVolume);
    } else if (element > 0 && element < kElementThrows.size()) {
        m_resources->audio.playNamed(kElementThrows[element], LevelSoundscape::kStepVolume);
    } else if (const auto sound = figure.throwSound();
               m_resources->sounds != nullptr && sound.has_value()) {
        try {
            m_resources->sounds->play(figure.voice().sequence(*sound), LevelSoundscape::kStepVolume,
                                      SoundCategory::Effects);
        } catch (const std::exception& e) {
            log::warn("Tower: throw sound: {}", e.what());
        }
    }
}

void PlayerArsenal::launchSuperShot(PlayerActor& actor, PlayerFigure* body,
                                    std::optional<Vec3> target) {
    if (!m_resources || body == nullptr) {
        return;
    }
    auto& inventory = actor.save().progress().inventory;
    const auto worn = PowerupEffects::of(inventory);
    if (!inventory.spendPowerup(powerup::kWeapon, powerup::kSuperShot, m_resources->tower)) {
        launchWeapon(actor, body, actor.facing(), 1, true, target);
        return;
    }
    s32 stat = 0;
    if (const auto* stats = m_resources->classes.stats(actor.save().character)) {
        const auto block = displayStats(*stats, experienceLevel(actor.save().experience()),
                                        actor.save().progress());
        stat = MissileSpec::byMagic(actor.save().character) ? block.magic() : block.strength();
    }
    MissileLaunch launch;
    launch.multiplayer =
        m_resources->multiplayer != nullptr ? *m_resources->multiplayer : MultiplayerMode::Normal;
    launch.owner = actor.player();
    launch.position = actor.followPoint() + actor.facing() * PlayerMissiles::kMuzzle;
    launch.direction = actor.facing();
    if (target && glm::length(*target - launch.position) > 0.001f) {
        launch.direction = glm::normalize(*target - launch.position);
    }
    launch.speed = PlayerMissiles::speedFor(stat);
    launch.damage = PlayerMissiles::damageFor(stat) * (m_resources->bossEncounter ? 1.5f : 2.0f);
    launch.flags = worn.weapon | powerup::kSuperShot | 0x20U;
    launch.spec = &MissileSpec::superShot();
    launch.playerHitGap = PlayerMissiles::hitGap(&m_resources->weapons, launch.spec->impactTree);
    launch.model = &m_superShot;
    launch.archive = &m_resources->weapons;
    launch.tree = "SUPERARROW";
    if (const auto texture = m_resources->weapons.textures.find("WEP_STREAK")) {
        launch.streak.texture =
            &m_resources->weapons.textures.texture(m_resources->device, *texture);
        // StartMissile's white Super Shot streak uses transparency 64. MBPolyInst
        // packs (255 - transparency) / 2, then doubles it for GX: opacity 190.
        launch.streak.color = Color::rgba(255, 255, 255, 190);
        if (const auto* stats = m_resources->classes.stats(actor.save().character)) {
            launch.streak.forward = stats->streakForward;
        }
    }
    for (const auto& direction : PlayerMissiles::spread(launch.direction, worn.shots())) {
        launch.velocity = direction * launch.speed;
        m_missiles.launch(launch);
    }
    m_resources->audio.playNamed("S_SUPERSHOT", LevelSoundscape::kStepVolume);
}

void PlayerArsenal::launchFamiliar(const PlayerActor& actor, PlayerFigure* body,
                                   std::optional<Vec3> target) {
    if (!m_resources || body == nullptr) {
        return;
    }
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    const bool phoenix = (worn.special & powerup::kPhoenix) != 0;
    if (!phoenix && (body->familiarTier() == 0 || !body->familiarMissile().bound())) {
        return;
    }
    const auto* stats = m_resources->classes.stats(actor.save().character);
    if (stats == nullptr) {
        return;
    }
    // PlayerMotion / Start familiar spit: radius one, three-second life, speed 35;
    // damage is 0.1 * (level - 25) + 2.5, independent of weapon powerups.
    static constexpr MissileSpec kLevelShot{"FAMILIAR_SPIT", {}, 1, 0, 10, true, {}};
    static constexpr MissileSpec kBossShot{"FAMILIAR_SPIT", {}, 1, 0, 0, true, {}};
    MissileLaunch launch;
    // Familiar spit's fixed collision mask excludes players in every game mode.
    launch.owner = actor.player();
    const f32 scale = PlayerFigure::bodyScale(actor.save(), worn);
    launch.position = Vec3{actor.transform() * Vec4{stats->familiarShotOffset * scale, 1}};
    launch.direction = actor.facing();
    launch.speed = 35;
    launch.damage = phoenix ? kPhoenixDamage
                            : 0.1f * static_cast<f32>(experienceLevel(actor.save().experience()));
    launch.flags = phoenix ? kPhoenixDamageFlags : 0;
    launch.spec = m_resources->bossEncounter ? &kBossShot : &kLevelShot;
    launch.model = phoenix ? &m_phoenixShot : &body->familiarMissile();
    launch.archive = phoenix ? &m_resources->weapons : body->effects();
    launch.tree = phoenix ? "PHOENIX_FBALL" : "FAMILIAR_SPIT";
    launch.wallSound = MissileWallSound::Silent;
    // CalcTargetDir normalizes horizontal displacement, uses a 50-unit/second
    // flight estimate, then StartFX scales the direction by 35.
    const Vec3 aim = target.value_or(launch.position + actor.facing() * 21.0f);
    const Vec3 delta = aim - launch.position;
    const f32 distance = std::hypot(delta.x, delta.z);
    const f32 inverse = distance > 0.001f ? 1.0f / distance : 1.0f;
    const f32 drop = m_resources->bossEncounter ? 0.0f : -0.5f;
    launch.velocity = Vec3{delta.x * inverse,
                           0.02f * (0.5f * launch.spec->weight * distance * 0.02f +
                                    (delta.y + drop) * 50.0f * inverse),
                           delta.z * inverse} *
                      launch.speed;
    m_missiles.launch(launch);
}

void PlayerArsenal::launchGauntlet(const PlayerActor& actor, PlayerFigure* body, bool left) {
    if (!m_resources || body == nullptr) {
        return;
    }
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    if ((worn.special & (left ? powerup::kLeftGauntlet : powerup::kRightGauntlet)) == 0) {
        return;
    }
    s32 stat = 0;
    if (const auto* stats = m_resources->classes.stats(actor.save().character)) {
        const auto block = displayStats(*stats, experienceLevel(actor.save().experience()),
                                        actor.save().progress());
        stat = MissileSpec::byMagic(actor.save().character) ? block.magic() : block.strength();
    }
    static constexpr std::array<MissileSpec, 2> kSpecs{
        {{"BOSSG_ACID", {}, 2, 0, 0, true}, {"BOSSG_ELEC", {}, 2, 0, 0, true}}};
    const usize which = left ? 1 : 0;
    MissileLaunch launch;
    launch.multiplayer =
        m_resources->multiplayer != nullptr ? *m_resources->multiplayer : MultiplayerMode::Normal;
    launch.owner = actor.player();
    launch.position = actor.followPoint() + actor.facing() * PlayerMissiles::kMuzzle;
    launch.speed = PlayerMissiles::speedFor(stat);
    launch.damage = PlayerMissiles::damageFor(stat);
    launch.flags = worn.weapon | (left ? 2U : 4U);
    launch.spec = &kSpecs[which];
    launch.playerHitGap = PlayerMissiles::hitGap(&m_resources->weapons, launch.spec->impactTree);
    launch.model = &m_gauntlets[which];
    launch.archive = &m_resources->weapons;
    launch.tree = kSpecs[which].model;
    if (auto* archive = body->effects()) {
        launch.textureLender = &archive->textures;
    }
    for (const auto& direction : PlayerMissiles::spread(actor.facing(), worn.shots())) {
        launch.velocity = direction * launch.speed;
        m_missiles.launch(launch);
    }
    m_resources->audio.playNamed(left ? "S_GAUNTLET1" : "S_GAUNTLET2");
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

void PlayerArsenal::burstPotion(s32 kind, const Vec3& position, f32 power, bool castSound) {
    if (!m_resources.has_value()) {
        return;
    }
    const PotionLook& look = potionLook(kind);
    if (m_resources->weapons.loaded()) {
        EffectTrees::Setting setting;
        setting.scale = std::min(kBurstPerPower * power, 1.0f);
        setting.light = EffectTrees::Light{DynamicLights::ofPotion(kind),
                                           DynamicLights::kMagicRadiusPerPower * power};
        m_resources->effects.startSet(m_resources->device, m_resources->weapons, look.burst,
                                      position, setting);
    }
    if (castSound) {
        m_resources->audio.playNamed(look.sound);
    }
}

void PlayerArsenal::presentImpact(const MissileImpact& impact, f32 playerDistance) {
    if (!m_resources) {
        return;
    }
    if (impact.potion != 0) {
        burstPotion(impact.potion, impact.position, impact.potency, !impact.liquid);
        if (impact.liquid) {
            m_resources->audio.playAt("S_SPLASH", impact.position, playerDistance,
                                      LevelSoundscape::kSplashLevel / 255.0f);
        }
        return;
    }
    if (impact.target >= 0) {
        return; // Target owners supply their own material/body feedback.
    }
    if (m_resources->weapons.loaded() && !impact.effect.empty()) {
        EffectTrees::Setting setting;
        setting.unlit = true;
        setting.depthWrite = false;
        if (impact.effect == "SPARKS") {
            setting.tint.a = 96;
        }
        m_resources->effects.startSet(m_resources->device, m_resources->weapons, impact.effect,
                                      impact.position, setting);
    }
    if (impact.liquid) {
        m_resources->audio.playAt("S_SPLASH", impact.position, playerDistance,
                                  LevelSoundscape::kSplashLevel / 255.0f);
        return;
    }
    switch (impact.wallSound) {
    case MissileWallSound::Level: m_resources->audio.playNamed(m_resources->wallHitSound); break;
    case MissileWallSound::ThreeWay: m_resources->audio.playNamed("S_3WAYAXE"); break;
    case MissileWallSound::FiveWay: m_resources->audio.playNamed("S_5WAYAXE"); break;
    case MissileWallSound::Silent: break;
    case MissileWallSound::Ricochet: m_resources->audio.playNamed("S_RICOCHET"); break;
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

f32 PlayerArsenal::potionPowerOf(const PlayerActor& actor, s32 kind) const {
    return magicPowerOf(actor) *
           damage::colourBonus(actor.save().color, static_cast<u32>(std::clamp(kind, 0, 4)));
}

std::optional<MissileImpact> PlayerArsenal::usePotion(PlayerActor& actor) {
    auto& inventory = actor.save().progress().inventory;
    if (!m_resources.has_value() || inventory.potions.empty()) {
        return std::nullopt;
    }
    const s32 stored = inventory.takePotion();
    const f32 bonus = damage::colourBonus(actor.save().color, static_cast<u32>(stored));
    const f32 power = potionPowerOf(actor, stored);
    const s32 kind = resolvePotionKind(stored);
    burstPotion(kind, actor.position(), power);
    MissileImpact burst;
    burst.position = actor.position();
    burst.owner = actor.player();
    burst.potion = kind;
    burst.potency = power;
    burst.damage = kPotionDamage * bonus; // start_magic: the magic stat sets the radius.
    burst.flags = damage::magicHeal(experienceLevel(actor.save().experience()));
    healingCast(actor, burst.potency);
    return burst;
}

s32 PlayerArsenal::resolvePotionKind(s32 kind) {
    if (kind != 0) {
        return kind;
    }
    const s32 resolved = m_nextPotionKind;
    m_nextPotionKind = m_nextPotionKind % 4 + 1;
    return resolved;
}

void PlayerArsenal::throwPotion(PlayerActor& actor, s32 heldTicks) {
    auto& inventory = actor.save().progress().inventory;
    if (!m_resources.has_value() || inventory.potions.empty()) {
        return;
    }
    const s32 stored = inventory.takePotion();
    const s32 kind = resolvePotionKind(stored);
    const f32 power = potionPowerOf(actor, stored);
    const Vec3 facing = actor.facing();
    MissileLaunch launch;
    launch.owner = actor.player();
    launch.direction = facing;
    launch.position =
        actor.position() + facing * kPotionHandReach + Vec3{0.0f, kPotionHandHeight, 0.0f};
    launch.velocity = Vec3{facing.x * kPotionLoft, kPotionLoft, facing.z * kPotionLoft} *
                      (kPotionToss + kPotionCharge * static_cast<f32>(std::max(0, heldTicks)));
    launch.potion = kind;
    launch.potency = kThrownShare * power;
    launch.damage =
        kPotionDamage * damage::colourBonus(actor.save().color, static_cast<u32>(stored));
    launch.flags = damage::magicHeal(experienceLevel(actor.save().experience()));
    launch.spec = &MissileSpec::potion();
    launch.model = &m_potionModels[static_cast<usize>(
        std::clamp(kind, 0, static_cast<s32>(m_potionModels.size()) - 1))];
    launch.archive = &m_resources->weapons;
    launch.tree = potionLook(kind).bottle;
    m_missiles.launch(launch);
    healingCast(actor, power);
}

void PlayerArsenal::healingCast(const PlayerActor& actor, f32 power) {
    if (!m_resources || m_resources->powerups == nullptr ||
        experienceLevel(actor.save().experience()) < 75) {
        return;
    }
    EffectTrees::Setting setting;
    setting.scale = std::min(kBurstPerPower * power, 1.0f);
    const u32 id = m_resources->effects.startSet(m_resources->device, *m_resources->powerups,
                                                 "MAGICHEALTH", actor.position(), setting);
    if (id != 0) {
        m_castEffects.push_back({actor.player(), id});
        followCaster(actor);
    }
}

void PlayerArsenal::followCaster(const PlayerActor& actor) {
    if (!m_resources) {
        return;
    }
    std::erase_if(m_castEffects, [&](const CastEffect& cast) {
        return !m_resources->effects.playing(cast.effect);
    });
    for (const auto& cast : m_castEffects) {
        if (cast.owner == actor.player()) {
            m_resources->effects.placeAt(cast.effect, actor.transform());
        }
    }
}
} // namespace gdl::game
