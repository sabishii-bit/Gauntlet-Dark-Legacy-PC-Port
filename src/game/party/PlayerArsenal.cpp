#include "game/party/PlayerArsenal.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

#include "game/characters/ClassData.h"
#include "game/characters/Progression.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/DynamicLights.h"
#include "game/effects/WeaponGlow.h"
#include "game/level/NativeStateResources.h"
#include "game/party/TargetAssist.h"
#include "game/players/PowerupEffects.h"
#include "game/players/TurboMove.h"
namespace gdl::game {
namespace {
constexpr f32 kPhoenixDamage = 10.0f;
constexpr u32 kPhoenixDamageFlags = damage::kFire | damage::kKnockBack;
static_assert(kPhoenixDamageFlags == 0x11U);

} // namespace
void PlayerArsenal::appendBindingDigest(NativeStateDigest& digest) const {
    digest.flag(m_resources.has_value());
    if (m_resources) {
        const auto& resources = *m_resources;
        digest.resource(&resources.collision);
        digest.flag(resources.weapons.loaded());
        if (resources.weapons.loaded()) {
            digest.resource(&resources.weapons);
        }
        digest.flag((resources.powerups != nullptr) && resources.powerups->loaded());
        if (resources.powerups && resources.powerups->loaded()) {
            digest.resource(resources.powerups);
        }
        digest.values(resources.wallHitSound, resources.tower, resources.bossEncounter);
        digest.flag(static_cast<bool>(resources.strikeMuzzleObject));
        digest.flag(resources.multiplayer != nullptr);
        if (resources.multiplayer) {
            digest.value(*resources.multiplayer);
        }
    }
}

bool PlayerArsenal::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("PlayerArsenal.native.1");
    appendBindingDigest(digest);
    digest.value(m_sim);
    return m_missiles.appendNativeDigest(digest) && digest.good();
}

bool PlayerArsenal::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("PlayerArsenal.native.1");
    appendBindingDigest(digest);
    digest.value(saved.m_state);
    return m_missiles.appendLocalDigest(digest, saved.m_missiles) && digest.good();
}

bool PlayerArsenal::registerNativeResources(NativeStateResources& resources) const {
    if (m_resources && m_resources->weapons.loaded()) {
        const auto& archive = m_resources->weapons;
        if (!resources.refreshArchive(archive)) {
            return false;
        }
        for (usize i = 0; i < m_potionModels.size(); ++i) {
            if (m_potionModels[i].bound() &&
                !resources.registerModel(m_potionModels[i], archive,
                                         "arsenal/potion/" + std::to_string(i))) {
                return false;
            }
        }
        for (usize i = 0; i < m_gauntlets.size(); ++i) {
            if (m_gauntlets[i].bound() &&
                !resources.registerModel(m_gauntlets[i], archive,
                                         "arsenal/gauntlet/" + std::to_string(i))) {
                return false;
            }
        }
        if ((m_superShot.bound() &&
             !resources.registerModel(m_superShot, archive, "arsenal/super")) ||
            (m_phoenixShot.bound() &&
             !resources.registerModel(m_phoenixShot, archive, "arsenal/phoenix"))) {
            return false;
        }
    }
    return m_missiles.registerNativeResources(resources) && resources.good();
}

PlayerArsenal::SoundCapture::SoundCapture(PlayerArsenal* arsenal, SoundObserver observer)
    : m_arsenal(arsenal) {
    if (m_arsenal != nullptr) {
        m_previous = std::exchange(m_arsenal->m_soundObserver, std::move(observer));
    }
}
PlayerArsenal::SoundCapture::~SoundCapture() {
    if (m_arsenal != nullptr) {
        m_arsenal->m_soundObserver = std::move(m_previous);
    }
}
void PlayerArsenal::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    saved.m_state = m_sim;
    m_missiles.saveLocal(saved.m_missiles);
}
bool PlayerArsenal::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_localBinding &&
           m_missiles.canRestoreLocal(saved.m_missiles);
}
bool PlayerArsenal::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    m_missiles.restoreLocal(saved.m_missiles);
    ++m_restoreGeneration;
    return true;
}
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
    m_localBinding = std::make_shared<const u8>(0);
    ++m_restoreGeneration;
    if (m_resources) {
        for (const auto& cast : m_sim.casts) {
            m_resources->effects.stop(cast.effect);
        }
    }
    m_sim.casts.clear();
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
MissileStreak PlayerArsenal::weaponStreak(const PlayerActor& actor, bool superShot) {
    MissileStreak streak;
    if (!m_resources) {
        return streak;
    }
    const auto texture = m_resources->weapons.textures.find("WEP_STREAK");
    if (!texture) {
        return streak;
    }
    return weaponStreak(
        actor, &m_resources->weapons.textures.texture(m_resources->device, *texture), superShot);
}
MissileStreak PlayerArsenal::weaponStreak(const PlayerActor& actor, const Texture* texture,
                                          bool superShot) const {
    MissileStreak streak;
    if (!m_resources || texture == nullptr) {
        return streak;
    }
    streak.texture = texture;
    // StartMissile's tables at GUNE5D 8011A178/8011A188: saturated costume colours,
    // not the pastel HUD tints. Wizard/sorceress families have transparency 192;
    // other classes and the white Super Shot use 64. MBPolyInst quantizes opacity
    // through (255 - transparency) / 2 before GX doubles it: 62 or 190.
    constexpr std::array kColors{Color::rgba(255, 255, 0, 190), Color::rgba(0, 0, 255, 190),
                                 Color::rgba(255, 0, 0, 190), Color::rgba(0, 255, 0, 190)};
    const auto& save = actor.save();
    streak.color = superShot
                       ? Color::rgba(255, 255, 255, 190)
                       : kColors[static_cast<usize>(std::clamp(save.color, 0, kColorCount - 1))];
    const s32 family = baseClassOf(save.character);
    if (!superShot && (family == kWizardClass || family == kSorceressClass)) {
        streak.color.a = 62;
    }
    if (const auto* stats = m_resources->classes.stats(save.character)) {
        streak.forward = stats->streakForward;
    }
    return streak;
}
std::optional<PlayerMissiles::WeaponThrow> PlayerArsenal::weaponThrow(const PlayerActor& actor,
                                                                      f32 attackSeconds) const {
    if (!m_resources) {
        return std::nullopt;
    }
    const CharacterSave& save = actor.save();
    const auto worn = PowerupEffects::of(save.progress().inventory);
    const bool pojo = (worn.special & powerup::kPojo) != 0;
    const ClassStats* stats = m_resources->classes.stats(save.character);
    s32 stat = 0;
    Vec3 hand{0.0f, 0.0f, 0.0f};
    if (stats != nullptr) {
        const StatBlock block = characterStats(*stats, save);
        stat = MissileSpec::byMagic(save.character) ? block.magic() : block.strength();
        hand = stats->weaponOffset;
    }
    // PlayerStartMissile's Pojo offset (GUNE5D 8011A1A8), relative to effectpos.
    // Its projectile still uses the underlying class's damage and flight tuning.
    if (pojo) {
        hand = Vec3{0, -0.5f, -1.25f};
    }
    PlayerMissiles::WeaponThrow weapon;
    weapon.multiplayer =
        m_resources->multiplayer != nullptr ? *m_resources->multiplayer : MultiplayerMode::Normal;
    weapon.owner = actor.player();
    weapon.character = save.character;
    weapon.position = actor.position();
    weapon.center = actor.followPoint();
    weapon.hand = hand;
    weapon.direction = actor.facing();
    weapon.speed = PlayerMissiles::speedFor(stat);
    weapon.damage = PlayerMissiles::damageFor(stat);
    weapon.flags = worn.weapon & ~damage::kSuperShot;
    weapon.attackSeconds = attackSeconds;
    weapon.shots = worn.shots();
    return weapon;
}

std::optional<PlayerArsenal::PreparedWeapon> PlayerArsenal::prepareWeapon(
    const PlayerActor& actor, const PlayerFigure& figure, const Vec3& direction, f32 scale,
    bool spreads, std::optional<Vec3> target, std::optional<MissileRelease> explicitRelease,
    bool fixedFacing, WeaponMode mode) const {
    const auto& animator = figure.animator();
    const auto weapon = weaponThrow(actor, animator.attackSeconds());
    if (!weapon) {
        return std::nullopt;
    }
    PreparedWeapon prepared;
    prepared.weapon = *weapon;
    prepared.weapon.direction = direction;
    prepared.weapon.scale = scale;
    prepared.weapon.shots = spreads ? prepared.weapon.shots : 1;
    prepared.weapon.target = target;
    prepared.weapon.fixedFacing = fixedFacing;
    // PlayerMotion 800850B8..800850C4 marks a completed PWRA_THROW with
    // both the large-weapon bit and knockback. Damage/visual scale alone
    // does not shove the target. Do not infer this from an arbitrary scale.
    if (animator.strongReleased()) {
        prepared.weapon.flags |= PlayerMissiles::kStrongThrowFlags;
    }
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    prepared.soundFlags = worn.weapon;
    prepared.pojo = (worn.special & powerup::kPojo) != 0;
    if (mode == WeaponMode::Volley) {
        // PlyrSfxDoDamage type 10 keeps the weapon element and Super Shot, but removes
        // fan modifiers. PlayerStartMissile mode zero launches from effectpos, not the hand.
        prepared.weapon.flags = worn.weapon & ~(damage::kThreeWayShot | damage::kFiveWayShot);
        prepared.weapon.shots = 1;
        prepared.weapon.hand = prepared.pojo ? Vec3{0, -0.5f, -1.25f} : Vec3{0};
    }
    prepared.missiles = PlayerMissiles::weaponLaunches(prepared.weapon);
    const auto& save = actor.save();
    if (!explicitRelease && (animator.released() || animator.strongReleased())) {
        explicitRelease = MissileRelease{save.character, animator.player().generation(),
                                         animator.strongReleased()};
    }
    bool nativeRelease = false;
    if (explicitRelease) {
        if (explicitRelease->emission == 0) {
            nativeRelease = (animator.released() || animator.strongReleased()) &&
                            explicitRelease->strong == animator.strongReleased();
        } else {
            const u32 slot = explicitRelease->emission >> 16;
            nativeRelease = PlayerAnimator::rollbackTurboAction(animator.action()) && slot > 0 &&
                            slot <= TurboMove::kMaxCheckpointRows && !explicitRelease->strong &&
                            prepared.weapon.shots == 1 && prepared.weapon.scale == 1 &&
                            !prepared.weapon.target;
        }
    }
    if (PlayerMissiles::replayModifiers(worn.weapon) && !prepared.pojo &&
        prepared.weapon.multiplayer == MultiplayerMode::Normal && nativeRelease &&
        explicitRelease && explicitRelease->character == save.character &&
        explicitRelease->generation != 0 &&
        explicitRelease->generation == animator.player().generation() &&
        explicitRelease->ordinal == 0) {
        for (usize ordinal = 0; ordinal < prepared.missiles.size(); ++ordinal) {
            auto release = *explicitRelease;
            release.ordinal = static_cast<u8>(ordinal);
            prepared.missiles[ordinal].release = release;
        }
    }
    return prepared;
}

std::optional<f32> PlayerArsenal::weaponTextureFrame(const PlayerFigure& figure) const {
    const auto* archive = figure.missileArchive();
    if (!m_resources || archive == nullptr) {
        return std::nullopt;
    }
    const std::array<TextureSet*, 1> lenders{&m_resources->weapons.textures};
    return m_missiles.visuals().textureFrame(*archive, lenders);
}

std::optional<PlayerArsenal::WeaponRider> PlayerArsenal::weaponRider(const PlayerActor& actor,
                                                                     PlayerFigure& figure) const {
    if (!m_resources) {
        return std::nullopt;
    }
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    const auto name = WeaponGlow::throwTree(WeaponGlow::elementOf(worn));
    auto* archive = name.empty() ? nullptr : figure.effects();
    const auto tree = archive != nullptr ? archive->trees.find(name) : std::nullopt;
    if (!tree) {
        return std::nullopt;
    }
    WeaponRider rider;
    rider.archive = archive;
    // The pointer is bound whenever the index it guards is set:
    // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
    rider.tree = &archive->trees.tree(*tree);
    rider.lenders.push_back(&m_resources->weapons.textures);
    rider.textureFrame = m_missiles.visuals().textureFrame(*archive, rider.lenders);
    const auto fallbacks = m_missiles.visuals().textureLenders();
    rider.lenders.insert(rider.lenders.end(), fallbacks.begin(), fallbacks.end());
    return rider;
}

void PlayerArsenal::bindWeaponAppearance(const PlayerActor& actor, PlayerFigure& figure,
                                         PreparedWeapon& prepared,
                                         const MissileStreak& streak) const {
    if (!m_resources || prepared.missiles.empty()) {
        return;
    }
    auto& launches = prepared.missiles;
    const auto& save = actor.save();
    const bool pojo = prepared.pojo;
    const bool superShot = prepared.missiles.front().spec == &MissileSpec::superShot();
    // The volley shares its muzzle and visual resources. Native launch math is
    // also used by physics-only replay, which never borrows these assets.
    MissileLaunch launch = launches.front();
    launch.playerHitGap = PlayerMissiles::hitGap(&m_resources->weapons, launch.spec->impactTree);
    // FamiliarSpit[4] is WEAPONS/PHOENIX_FBALL, not the earned familiar's shot.
    launch.model = pojo ? &m_phoenixShot : (superShot ? &m_superShot : &figure.missile());
    launch.archive = pojo || superShot ? &m_resources->weapons : figure.missileArchive();
    launch.tree = pojo ? "PHOENIX_FBALL" : (superShot ? "SUPERARROW" : figure.missileTree());
    launch.streak = streak;
    launch.textureLender = pojo || superShot ? nullptr : &m_resources->weapons.textures;
    // An elemental weapon's throw carries the element's WEAP_TW effect of the costume
    // colour's effects (PlayerStartMissile, combat.c 1030); the wizards and sorceresses
    // throw that effect alone, their weapon unseen.
    const std::string rider =
        WeaponGlow::throwTree(WeaponGlow::elementOf(PowerupEffects::of(save.progress().inventory)));
    if (ItemArchive* effects = rider.empty() || superShot ? nullptr : figure.effects();
        effects != nullptr && effects->trees.find(rider).has_value()) {
        launch.riderArchive = effects;
        // The descriptor can outlive this helper; borrow the authenticated
        // archive name, not the temporary lookup string.
        launch.riderTree = effects->trees.tree(*effects->trees.find(rider)).name;
        if (WeaponGlow::throwsEffectAlone(save.character)) {
            launch.model = nullptr;
            launch.archive = nullptr;
            launch.tree = {};
        }
    }
    for (auto& member : launches) {
        auto appearance = launch;
        appearance.direction = member.direction;
        appearance.velocity = member.velocity;
        appearance.wallSound = member.wallSound;
        appearance.release = member.release;
        member = appearance;
    }
}

PlayerArsenal::WeaponCue PlayerArsenal::weaponCue(u32 weapon, const PlayerFigure& figure) {
    // GUNE5D PlayerMotion -> AudioPlayerEatSFX (8009F638) also follows this
    // weapon/class dispatch for Pojo. S_POJOTURBO belongs to breath, not shots.
    constexpr std::array<std::string_view, 5> kElementThrows{"", "S_AMULETFIRE", "S_AMULETLIGHTNI",
                                                             "S_AMULETLIGHT", "S_AMULETACID"};
    constexpr u32 kSpecialThrows =
        damage::kThreeWayShot | damage::kFiveWayShot | damage::kSuperShot;
    const auto element = static_cast<usize>(weapon & damage::kElementMask);
    if ((weapon & kSpecialThrows) != 0) {
        return {.name = "S_SUPERSHOT"};
    }
    if (element > 0 && element < kElementThrows.size()) {
        return {.name = kElementThrows[element]};
    }
    return {.call = figure.throwSound()};
}

Vec3 PlayerArsenal::weaponMuzzle(const MissileLaunch& first,
                                 std::vector<WallContact>* contacts) const {
    const f32 radius = first.spec->radius;
    // PlayerStartMissile clears the muzzle with WeaponWallCollide, so the fences only the
    // party walks against (a lava ledge's) are no obstruction to the shot.
    return m_resources->collision.resolveWalls(
        first.position, radius, first.position.y - radius * 0.5f, first.position.y + radius * 0.5f,
        contacts, std::nullopt, WorldCollision::WallPush::All, WorldCollision::kMissileQueryFlags);
}

void PlayerArsenal::launchWeapon(const PlayerActor& actor, PlayerFigure* body,
                                 const Vec3& direction, f32 scale, bool spreads,
                                 std::optional<Vec3> target,
                                 std::optional<MissileRelease> explicitRelease, bool fixedFacing,
                                 WeaponMode mode) {
    if (!m_resources || body == nullptr) {
        return;
    }
    PlayerFigure& figure = *body;
    auto prepared = prepareWeapon(actor, figure, direction, scale, spreads, target, explicitRelease,
                                  fixedFacing, mode);
    if (!prepared || prepared->missiles.empty()) {
        return;
    }
    const bool superShot = prepared->missiles.front().spec == &MissileSpec::superShot();
    bindWeaponAppearance(actor, figure, *prepared, weaponStreak(actor, superShot));
    const auto& launch = prepared->missiles.front();
    // An obstructed muzzle still produces a world impact, without a flying weapon.
    std::vector<WallContact> muzzleContacts;
    const Vec3 clear = superShot ? launch.position : weaponMuzzle(launch, &muzzleContacts);
    if (glm::distance(clear, launch.position) > 1e-4f) {
        // A native imported shootwall is both an item and world geometry.
        // Let its authored owner receive this physical contact even though no
        // flying missile is created. An unrelated wall must still stop it.
        if (m_resources->strikeMuzzleObject && !muzzleContacts.empty() &&
            muzzleContacts.front().object >= 0) {
            m_resources->strikeMuzzleObject(muzzleContacts.front().object, launch.damage,
                                            launch.flags, launch.owner);
        }
        presentImpact({.position = clear, .owner = launch.owner});
        return;
    }
    const auto release = launch.release;
    bool launched = false;
    for (const auto& member : prepared->missiles) {
        if (m_missiles.launch(member)) {
            launched = true;
        }
    }
    const auto cue = weaponCue(prepared->soundFlags, figure);
    const auto soundScope =
        m_resources->audio.soundScope({15, 0, 1, static_cast<u8>(actor.player())}, 1);
    SoundHandle handle = kNoSound;
    if (!cue.name.empty()) {
        handle = m_resources->audio.playNamed(cue.name, LevelSoundscape::kStepVolume);
    } else if (cue.call) {
        try {
            handle = m_resources->audio.playFromCall(figure.voice(), *cue.call,
                                                     LevelSoundscape::kStepVolume);
        } catch (const SceneSoundEvents::Unsupported&) {
            throw;
        } catch (const std::exception& e) {
            log::warn("Tower: throw sound: {}", e.what());
        }
    }
    if (launched && release && handle != kNoSound && m_soundObserver) {
        m_soundObserver({handle, actor.player(), *release});
    }
}

void PlayerArsenal::launchSuperShot(PlayerActor& actor, PlayerFigure* body,
                                    std::optional<Vec3> target) {
    if (!m_resources || body == nullptr) {
        return;
    }
    auto& inventory = actor.save().progress().inventory;
    const auto worn = PowerupEffects::of(inventory);
    if (!inventory.spendPowerup(powerup::kWeapon, damage::kSuperShot, m_resources->tower)) {
        launchWeapon(actor, body, actor.facing(), 1, true, target);
        return;
    }
    s32 stat = 0;
    if (const auto* stats = m_resources->classes.stats(actor.save().character)) {
        const auto block = characterStats(*stats, actor.save());
        stat = MissileSpec::byMagic(actor.save().character) ? block.magic() : block.strength();
    }
    MissileLaunch launch;
    launch.multiplayer =
        m_resources->multiplayer != nullptr ? *m_resources->multiplayer : MultiplayerMode::Normal;
    launch.owner = actor.player();
    launch.position = actor.followPoint() + actor.facing() * PlayerMissiles::kMuzzle;
    const bool pojo = (worn.special & powerup::kPojo) != 0;
    if (pojo) {
        launch.position += Vec3{0, -0.5f, 0} - actor.facing() * 1.25f;
    }
    launch.direction = actor.facing();
    // ModifyPlayerDpos bypasses target correction for the Super Shot damage flag.
    launch.speed = PlayerMissiles::speedFor(stat);
    launch.damage = PlayerMissiles::damageFor(stat) * (m_resources->bossEncounter ? 1.5f : 2.0f);
    launch.flags = worn.weapon | damage::kSuperShot | damage::kKnockDown;
    launch.spec = &MissileSpec::superShot();
    launch.playerHitGap = PlayerMissiles::hitGap(&m_resources->weapons, launch.spec->impactTree);
    launch.model = pojo ? &m_phoenixShot : &m_superShot;
    launch.archive = &m_resources->weapons;
    launch.tree = pojo ? "PHOENIX_FBALL" : "SUPERARROW";
    launch.streak = weaponStreak(actor, true);
    for (const auto& direction : PlayerMissiles::spread(launch.direction, worn.shots())) {
        launch.velocity = direction * launch.speed;
        m_missiles.launch(launch);
    }
    m_resources->audio.playNamed("S_SUPERSHOT", LevelSoundscape::kStepVolume);
}

const MissileSpec& PlayerArsenal::familiarSpec(bool boss) {
    static constexpr MissileSpec kLevelShot{"FAMILIAR_SPIT", {}, 1, 0, 10, true, {}};
    static constexpr MissileSpec kBossShot{"FAMILIAR_SPIT", {}, 1, 0, 0, true, {}};
    return boss ? kBossShot : kLevelShot;
}

std::optional<std::vector<MissileLaunch>> PlayerArsenal::prepareFamiliar(const PlayerActor& actor,
                                                                         PlayerFigure& figure,
                                                                         std::optional<Vec3> target,
                                                                         bool fixedFacing) const {
    if (!m_resources) {
        return std::nullopt;
    }
    std::vector<MissileLaunch> shots;
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    const bool phoenix = (worn.special & powerup::kPhoenix) != 0;
    const bool familiar = figure.familiarTier() > 0 && figure.familiarMissile().bound();
    if (!phoenix && !familiar) {
        return shots;
    }
    const auto* stats = m_resources->classes.stats(actor.save().character);
    if (stats == nullptr) {
        return std::nullopt;
    }
    // PlayerMotion / Start familiar spit: radius one, three-second life, speed 35;
    // damage is 0.1 * (level - 25) + 2.5, independent of weapon powerups.
    MissileLaunch launch;
    // Companion shots do not hit players or detonate collectible potions.
    launch.owner = actor.player();
    launch.breaksPotions = false;
    const Mat4 placement = PlayerFigure::bodyPlacement(actor.transform(), actor.save(), worn);
    launch.position = Vec3{placement * Vec4{stats->familiarShotOffset, 1}};
    launch.direction = actor.facing();
    launch.speed = 35;
    launch.spec = &familiarSpec(m_resources->bossEncounter);
    launch.wallSound = MissileWallSound::Silent;
    // CalcTargetDir normalizes horizontal displacement, uses a 50-unit/second
    // flight estimate, then StartFX scales the direction by 35.
    const auto launchShot = [&] {
        const auto corrected =
            target && fixedFacing
                ? TargetAssist::heightTarget(launch.position, *target, actor.facing())
                : target;
        const Vec3 aim = corrected.value_or(launch.position + actor.facing() * 21.0f);
        const Vec3 delta = aim - launch.position;
        const f32 distance = portable::hypot(delta.x, delta.z);
        const f32 inverse = distance > 0.001f ? 1.0f / distance : 1.0f;
        const f32 drop = m_resources->bossEncounter ? 0.0f : -0.5f;
        launch.velocity = Vec3{delta.x * inverse,
                               0.02f * (0.5f * launch.spec->weight * distance * 0.02f +
                                        (delta.y + drop) * 50.0f * inverse),
                               delta.z * inverse} *
                          launch.speed;
        shots.push_back(launch);
    };
    // Both companions share the release and aim, but retain independent damage and visuals.
    if (familiar) {
        launch.damage = 0.1f * static_cast<f32>(experienceLevel(actor.save().experience()));
        launch.model = &figure.familiarMissile();
        launch.archive = figure.loadedEffects();
        launch.tree = "FAMILIAR_SPIT";
        launchShot();
    }
    if (phoenix) {
        launch.position =
            Vec3{figure.phoenixAttachment(placement) * Vec4{stats->familiarShotOffset, 1}};
        launch.damage = kPhoenixDamage;
        launch.flags = kPhoenixDamageFlags;
        launch.model = &m_phoenixShot;
        launch.archive = &m_resources->weapons;
        launch.tree = "PHOENIX_FBALL";
        launchShot();
    }
    return shots;
}

void PlayerArsenal::launchFamiliar(const PlayerActor& actor, PlayerFigure* body,
                                   std::optional<Vec3> target, bool fixedFacing) {
    if (!m_resources || body == nullptr) {
        return;
    }
    // Keep the native lazy archive boundary outside the callback-free preparation.
    if (m_resources->classes.stats(actor.save().character) != nullptr && body->familiarTier() > 0 &&
        body->familiarMissile().bound()) {
        body->effects();
    }
    if (const auto shots = prepareFamiliar(actor, *body, target, fixedFacing)) {
        for (const auto& launch : *shots) {
            m_missiles.launch(launch);
        }
    }
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
        const auto block = characterStats(*stats, actor.save());
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
    launch.streak = weaponStreak(actor, (launch.flags & damage::kSuperShot) != 0);
    if (auto* archive = body->effects()) {
        launch.textureLender = &archive->textures;
    }
    for (const auto& direction : PlayerMissiles::spread(actor.facing(), worn.shots())) {
        launch.velocity = direction * launch.speed;
        m_missiles.launch(launch);
    }
    m_resources->audio.playNamed(left ? "S_GAUNTLET1" : "S_GAUNTLET2");
}

const TreeModel* PlayerArsenal::potionModel(s32 kind) const {
    if (kind < 0 || static_cast<usize>(kind) >= m_potionModels.size()) {
        return nullptr;
    }
    const auto& model = m_potionModels[static_cast<usize>(kind)];
    return model.bound() ? &model : nullptr;
}

void PlayerArsenal::loadPotionModels() {
    if (!m_resources.has_value() || !m_resources->weapons.loaded()) {
        return;
    }
    for (usize kind = 0; kind < m_potionModels.size(); ++kind) {
        if (const auto tree = m_resources->weapons.trees.find(
                PotionEmission::look(static_cast<s32>(kind)).bottle);
            tree.has_value()) {
            m_potionModels[kind].bind(m_resources->weapons.trees.tree(*tree),
                                      m_resources->weapons.models, m_resources->weapons.textures,
                                      m_resources->device);
        }
    }
}

void PlayerArsenal::burstPotion(s32 kind, const Vec3& position, f32 power, bool castSound) {
    PotionEmission::Context context;
    context.resolvedKind = kind;
    context.position = position;
    context.power = power;
    presentPotion(PotionEmission::burst(context), castSound);
}

void PlayerArsenal::presentPotion(const PotionEmission::Burst& burst, bool castSound) {
    if (!m_resources.has_value()) {
        return;
    }
    if (m_resources->weapons.loaded()) {
        m_resources->effects.startSet(m_resources->device, m_resources->weapons, burst.tree,
                                      burst.impact.position, burst.setting);
    }
    if (castSound) {
        m_resources->audio.playNamed(burst.sound);
    }
}

PlayerArsenal::ImpactPresentation PlayerArsenal::impactPresentation(const MissileImpact& impact,
                                                                    f32 playerDistance) const {
    ImpactPresentation result;
    result.position = impact.position;
    if (!m_resources || impact.potion != 0 || impact.target >= 0) {
        return result; // Target owners supply their own material/body feedback.
    }
    if (m_resources->weapons.loaded() && !impact.effect.empty()) {
        result.archive = &m_resources->weapons;
        result.tree = impact.effect;
        result.setting.unlit = true;
        result.setting.depthWrite = false;
        if (impact.effect == "SPARKS") {
            result.setting.tint.a = 96;
        }
    }
    if (impact.liquid) {
        result.cue = ImpactCue{"S_SPLASH", impact.position, playerDistance,
                               LevelSoundscape::kSplashLevel / 255.0f};
        return result;
    }
    std::string_view sound;
    switch (impact.wallSound) {
    case MissileWallSound::Level: sound = m_resources->wallHitSound; break;
    case MissileWallSound::ThreeWay: sound = "S_3WAYAXE"; break;
    case MissileWallSound::FiveWay: sound = "S_5WAYAXE"; break;
    case MissileWallSound::Silent: break;
    case MissileWallSound::Ricochet: sound = "S_RICOCHET"; break;
    }
    if (!sound.empty()) {
        result.cue = ImpactCue{std::string{sound}, impact.position, std::nullopt, 1};
    }
    return result;
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
    const auto emission = impactPresentation(impact, playerDistance);
    if (emission.archive != nullptr) {
        m_resources->effects.startSet(m_resources->device, *emission.archive, emission.tree,
                                      emission.position, emission.setting);
    }
    if (emission.cue) {
        const auto& cue = *emission.cue;
        if (cue.playerDistance) {
            m_resources->audio.playAt(cue.name, cue.position, *cue.playerDistance, cue.volume);
        } else {
            m_resources->audio.playNamed(cue.name, cue.volume);
        }
    }
}

f32 PlayerArsenal::magicPowerOf(const PlayerActor& actor) const {
    const CharacterSave& save = actor.save();
    const ClassStats* stats =
        m_resources.has_value() ? m_resources->classes.stats(save.character) : nullptr;
    const s32 magic = stats != nullptr ? characterStats(*stats, save).magic() : 0;
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
    const auto burst =
        PotionEmission::burst({actor.player(), actor.position(), actor.facing(), kind, power, bonus,
                               damage::magicHeal(experienceLevel(actor.save().experience()))});
    presentPotion(burst, true);
    healingCast(actor, burst.impact.potency);
    return burst.impact;
}

s32 PlayerArsenal::resolvePotionKind(s32 kind) {
    if (kind != 0) {
        return kind;
    }
    const s32 resolved = m_sim.nextPotionKind;
    m_sim.nextPotionKind = m_sim.nextPotionKind % 4 + 1;
    return resolved;
}

void PlayerArsenal::throwPotion(PlayerActor& actor, s32 heldTicks, u64 generation) {
    auto& inventory = actor.save().progress().inventory;
    if (!m_resources.has_value() || inventory.potions.empty()) {
        return;
    }
    const s32 stored = inventory.takePotion();
    const s32 kind = resolvePotionKind(stored);
    const f32 power = potionPowerOf(actor, stored);
    auto launch = PotionEmission::throwPotion(
        {actor.player(), actor.position(), actor.facing(), kind, power,
         damage::colourBonus(actor.save().color, static_cast<u32>(stored)),
         damage::magicHeal(experienceLevel(actor.save().experience()))},
        heldTicks);
    launch.model = &m_potionModels[static_cast<usize>(
        std::clamp(kind, 0, static_cast<s32>(m_potionModels.size()) - 1))];
    launch.archive = &m_resources->weapons;
    if (generation != 0) {
        launch.potionRelease = MissilePotionRelease{generation};
    }
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
        m_sim.casts.push_back({actor.player(), id});
        followCaster(actor);
    }
}

void PlayerArsenal::followCaster(const PlayerActor& actor) {
    if (!m_resources) {
        return;
    }
    std::erase_if(m_sim.casts, [&](const CastEffect& cast) {
        return !m_resources->effects.playing(cast.effect);
    });
    for (const auto& cast : m_sim.casts) {
        if (cast.owner == actor.player()) {
            m_resources->effects.placeAt(cast.effect, actor.transform());
        }
    }
}
} // namespace gdl::game
