#include "game/party/PlayerMissiles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/combat/DamageTypes.h"
#include "game/party/TargetAssist.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

namespace {

constexpr f32 kTumble = 18.85f; ///< three turns a second
constexpr std::string_view kCostumeTiers = "0000000000";
constexpr std::string_view kStaffTiers = "1112223333";
constexpr std::string_view kBombTiers = "1111112233";
constexpr std::string_view kFirstTiers = "1111111111";
constexpr usize kFamilyCount = 8;
constexpr s32 kSumner = 16;
constexpr s32 kWizard = 2;
constexpr f32 kReflectedDamage = 15.0f;
constexpr f32 kReflectionTime = 1.0f;
constexpr f32 kPlayerPassThroughGap = 1.0f;

/** The sixteen classes' throws: the eight to start with, then the eight that shadow them. */
constexpr std::array<MissileSpec, 16> kSpecs{{
    {"AXE", kCostumeTiers, 1.0f, kTumble, 12.0f, false},
    {"SWD", kCostumeTiers, 1.0f, kTumble, 8.0f, false},
    {"STF", kStaffTiers, 1.2f, 0.0f, 8.0f, true},
    {"BOW", kStaffTiers, 0.7f, 0.0f, 8.0f, true},
    {"HAM", kCostumeTiers, 1.0f, kTumble, 20.0f, false},
    {"MAC", kCostumeTiers, 1.0f, kTumble, 8.0f, false},
    {"WND", kStaffTiers, 1.2f, 0.0f, 8.0f, true},
    {"BOM", kBombTiers, 0.7f, 0.0f, 8.0f, false, "EXPSMALL"},
    {"MIN", kFirstTiers, 1.0f, kTumble, 12.0f, false},
    {"FAL", kFirstTiers, 1.0f, kTumble, 8.0f, false},
    {"STF", kStaffTiers, 1.2f, 0.0f, 8.0f, true},
    {"BOW", kStaffTiers, 0.7f, 0.0f, 8.0f, true},
    {"OGR", kFirstTiers, 1.0f, kTumble, 20.0f, false},
    {"UNI", kFirstTiers, 1.0f, kTumble, 8.0f, false},
    {"WND", kStaffTiers, 1.2f, 0.0f, 8.0f, true},
    {"BOM", kBombTiers, 0.7f, 0.0f, 8.0f, false, "EXPSMALL"},
}};

usize specIndex(s32 classIndex) {
    if (classIndex == kSumner) {
        return kWizard;
    }
    return static_cast<usize>(std::clamp(classIndex, 0, static_cast<s32>(kSpecs.size()) - 1));
}

void restoreMotion(PlayerMissiles::Missile& shot, const PlayerMissiles::FlightCheckpoint& state) {
    shot.position = state.position;
    shot.velocity = state.velocity;
    shot.tumble = state.tumble;
    shot.age = state.age;
    shot.scale = state.scale;
    shot.continuity = state.continuity;
    shot.previousPosition = shot.position;
    shot.previousVelocity = shot.velocity;
    shot.previousTumble = shot.tumble;
    shot.previousAge = shot.age;
    shot.presentationCaptured = false;
}

} // namespace

const MissileSpec& MissileSpec::of(s32 classIndex) {
    return kSpecs[specIndex(classIndex)];
}

std::string MissileSpec::treeName(s32 classIndex, s32 level, bool* inCostume) {
    const MissileSpec& spec = of(classIndex);
    const auto tier = static_cast<usize>(std::clamp(level / 10, 0, 9));
    const char mark = spec.tiers[tier];
    if (inCostume != nullptr) {
        *inCostume = mark == '0';
    }
    return std::format("{}_THROW{}", spec.model, mark);
}

const MissileSpec& MissileSpec::potion() {
    static constexpr MissileSpec kPotion{"POT", kCostumeTiers, 0.5f, kTumble, 100.0f, false};
    return kPotion;
}

const MissileSpec& MissileSpec::superShot() {
    static constexpr MissileSpec kSuper{"SUPERARROW", {}, 5.0f, 0.0f, 0.0f, true};
    return kSuper;
}

bool MissileSpec::byMagic(s32 classIndex) {
    const usize family = specIndex(classIndex) % kFamilyCount;
    return family == 2 || family == 6;
}

f32 PlayerMissiles::speedFor(s32 stat) {
    return kSlowest +
           kStatScale * static_cast<f32>(std::clamp(stat, 0, 1000)) * (kFastest - kSlowest);
}

bool PlayerMissiles::replayModifiers(u32 flags) {
    return (flags & ~kReplayModifiers) == 0 && (flags & damage::kElementMask) <= damage::kAcid;
}

f32 PlayerMissiles::reachFor(f32 attackSeconds) {
    return kReach + kReachPerSecond * std::clamp(attackSeconds - kHoldDelay, 0.0f, kHoldMost);
}

Vec3 PlayerMissiles::launchVelocity(const Vec3& direction, f32 speed, f32 reach, f32 weight) {
    // Over the time the reach takes, gravity is cancelled and the drop made up.
    const f32 flight = reach / speed;
    return Vec3{direction.x * speed, 0.5f * weight * flight - kDrop / flight, direction.z * speed};
}

std::vector<MissileLaunch> PlayerMissiles::weaponLaunches(const WeaponThrow& weapon) {
    Vec3 facing = weapon.direction;
    if (weapon.target && !weapon.fixedFacing) {
        const Vec3 offset = *weapon.target - weapon.position;
        const f32 distance = portable::hypot(offset.x, offset.z);
        if (distance > 1e-5f) {
            facing = Vec3{offset.x / distance, 0, offset.z / distance};
        }
    }
    const Vec3 side{facing.z, 0, -facing.x};
    MissileLaunch launch;
    launch.owner = weapon.owner;
    launch.scale = weapon.scale;
    launch.position = weapon.center + side * weapon.hand.x + Vec3{0, weapon.hand.y, 0} +
                      facing * (weapon.hand.z + kMuzzle);
    launch.speed = weapon.speed;
    launch.damage = weapon.damage * weapon.scale;
    launch.flags = weapon.flags;
    launch.multiplayer = weapon.multiplayer;
    launch.reach = reachFor(weapon.attackSeconds);
    const bool superShot =
        (weapon.flags & damage::kSuperShot) != 0 && (weapon.flags & damage::kTurbo) == 0;
    launch.spec = superShot ? &MissileSpec::superShot() : &MissileSpec::of(weapon.character);
    if (weapon.shots == 5) {
        launch.wallSound = MissileWallSound::FiveWay;
    } else if (weapon.shots == 3) {
        launch.wallSound = MissileWallSound::ThreeWay;
    }
    const auto target = weapon.target && weapon.fixedFacing
                            ? TargetAssist::heightTarget(launch.position, *weapon.target, facing)
                            : weapon.target;
    std::vector<MissileLaunch> launches;
    for (const Vec3& way : spread(facing, weapon.shots)) {
        launch.direction = way;
        if (superShot) {
            launch.velocity = way * launch.speed;
        } else if (target) {
            const Vec3 aim =
                TargetAssist::velocity(launch.position, *target, launch.speed, launch.spec->weight);
            // Each spread keeps its authored bearing instead of converging on the target.
            const f32 turn = portable::atan2(way.x, way.z) - portable::atan2(facing.x, facing.z);
            launch.velocity = Vec3{aim.x * portable::cos(turn) + aim.z * portable::sin(turn), aim.y,
                                   aim.z * portable::cos(turn) - aim.x * portable::sin(turn)};
        }
        launches.push_back(launch);
        launch.wallSound = MissileWallSound::Silent;
    }
    return launches;
}

bool PlayerMissiles::FlightCheckpoint::valid() const {
    constexpr f32 kPositionLimit = 1'000'000;
    constexpr f32 kVelocityLimit = 10'000;
    constexpr f32 kTumbleLimit = 1'000'000;
    constexpr f32 kScaleLimit = 100;
    const auto within = [](const Vec3& value, f32 limit) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
               std::abs(value.x) <= limit && std::abs(value.y) <= limit &&
               std::abs(value.z) <= limit;
    };
    return within(position, kPositionLimit) && within(velocity, kVelocityLimit) &&
           std::isfinite(tumble) && std::abs(tumble) <= kTumbleLimit && std::isfinite(age) &&
           age >= 0 && age < kLifeSeconds && std::isfinite(scale) && scale > 0 &&
           scale <= kScaleLimit && continuity != 0;
}

bool PlayerMissiles::ParticleFlight::valid() const {
    if (!birth.valid() || birth.age != 0 || birth.tumble != 0 || birth.continuity != 1 ||
        !std::isfinite(stepSeconds) || stepSeconds < 1.0f / 240 || stepSeconds > 0.25f ||
        activeSteps > totalSteps || totalSteps > 4801 ||
        (retiring ? activeSteps == totalSteps : activeSteps != totalSteps)) {
        return false;
    }
    return activeSteps <= static_cast<u32>(std::ceil(kLifeSeconds / stepSeconds)) &&
           totalSteps <= static_cast<u32>(std::ceil(20.0f / stepSeconds)) + 1;
}

const PlayerMissiles::VisualHistory* PlayerMissiles::visualHistory(u32 effect) const {
    if (effect == 0) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(m_sim.visualHistory, [effect](const auto& history) {
        return history.effect == effect || history.rider == effect;
    });
    return found != m_sim.visualHistory.end() ? &*found : nullptr;
}

PlayerMissiles::FlightCheckpoint PlayerMissiles::flightCheckpoint(const Missile& missile) {
    return {missile.position, missile.velocity, missile.tumble,
            missile.age,      missile.scale,    missile.continuity};
}

bool PlayerMissiles::restoreFlight(u64 instance, const FlightCheckpoint& checkpoint) {
    if (m_device != nullptr || !checkpoint.valid()) {
        return false;
    }
    const auto shot = std::ranges::find(m_sim.missiles, instance, &Missile::instance);
    if (shot == m_sim.missiles.end() || shot->potion != 0 || shot->potionRelease ||
        shot->flags != 0 || shot->multiplayer != MultiplayerMode::Normal ||
        !shot->pierced.empty() || !shot->playerContacts.empty()) {
        return false;
    }
    restoreMotion(*shot, checkpoint);
    return true;
}

bool PlayerMissiles::restorePotionFlight(u64 instance, const FlightCheckpoint& checkpoint) {
    if (m_device != nullptr || !checkpoint.valid()) {
        return false;
    }
    const auto shot = std::ranges::find(m_sim.missiles, instance, &Missile::instance);
    if (shot == m_sim.missiles.end() || shot->potion < 1 || shot->potion > 4 ||
        shot->spec != &MissileSpec::potion() || (shot->flags & ~damage::kHeal) != 0 ||
        shot->release || shot->multiplayer != MultiplayerMode::Normal || !shot->pierced.empty() ||
        !shot->playerContacts.empty()) {
        return false;
    }
    restoreMotion(*shot, checkpoint);
    return true;
}

bool PlayerMissiles::restoreVisualFlight(u64 instance, const ParticleFlight& history) {
    if (m_device != nullptr || !history.valid() || history.retiring) {
        return false;
    }
    const auto found = std::ranges::find(m_sim.visualHistory, instance, &VisualHistory::instance);
    if (found == m_sim.visualHistory.end()) {
        return false;
    }
    found->flight = history;
    return true;
}

bool PlayerMissiles::launch(const MissileLaunch& launch) {
    return launchWithVisuals(launch, nullptr, nullptr, false);
}

bool PlayerMissiles::launchPrepared(const MissileLaunch& launch, const EffectTrees::Prepared& body,
                                    const EffectTrees::Prepared* rider) {
    const auto matches = [&](const EffectTrees::Prepared& value, ItemArchive* archive,
                             std::string_view name) {
        if (m_device == nullptr || value.device != m_device || archive == nullptr ||
            value.archive != archive || value.tree == nullptr || value.tree->name != name ||
            value.model == nullptr || value.particles == nullptr || value.textures == nullptr ||
            value.descriptor != nullptr) {
            return false;
        }
        const auto index = archive->trees.find(name);
        if (!index || &archive->trees.tree(*index) != value.tree) {
            return false;
        }
        const usize explicitCount = launch.textureLender != nullptr ? 1 : 0;
        const auto fallback = m_visuals.textureLenders();
        return value.lenders.size() == explicitCount + fallback.size() &&
               (explicitCount == 0 || value.lenders.front() == launch.textureLender) &&
               std::ranges::equal(value.lenders.subspan(explicitCount), fallback);
    };
    if (!matches(body, launch.archive, launch.tree) ||
        (rider == nullptr ? launch.riderArchive != nullptr || !launch.riderTree.empty()
                          : !matches(*rider, launch.riderArchive, launch.riderTree))) {
        return false;
    }
    return launchWithVisuals(launch, &body, rider, true);
}

bool PlayerMissiles::launchWithVisuals(const MissileLaunch& launch,
                                       const EffectTrees::Prepared* body,
                                       const EffectTrees::Prepared* rider, bool prepared) {
    if (m_sim.nextInstance == 0 || launch.spec == nullptr || launch.speed <= 0.0f ||
        launch.reach <= 0.0f) {
        return false;
    }
    Missile missile;
    missile.instance = m_sim.nextInstance++;
    missile.owner = launch.owner;
    missile.position = launch.position;
    missile.velocity = launch.velocity.value_or(
        launchVelocity(launch.direction, launch.speed, launch.reach, launch.spec->weight));
    missile.potion = launch.potion;
    missile.potency = launch.potency;
    missile.damage = launch.damage;
    missile.scale = launch.scale;
    missile.spec = launch.spec;
    missile.model = launch.model;
    missile.wallSound = launch.wallSound;
    missile.flags = launch.flags;
    missile.streak = launch.streak;
    missile.multiplayer = launch.multiplayer;
    missile.playerHitGap = launch.playerHitGap;
    missile.breaksPotions = launch.breaksPotions;
    missile.release = launch.release;
    missile.potionRelease = launch.potionRelease;
    if (m_device != nullptr && launch.archive != nullptr && !launch.tree.empty()) {
        EffectTrees::Setting setting;
        setting.persistent = true;
        const std::array<TextureSet*, 1> lenders{launch.textureLender};
        missile.effect = prepared ? m_visuals.startPrepared(*body, missile.position, setting)
                                  : m_visuals.startSet(*m_device, *launch.archive, launch.tree,
                                                       missile.position, setting,
                                                       launch.textureLender != nullptr
                                                           ? std::span{lenders}
                                                           : std::span<TextureSet* const>{});
        if (prepared && missile.effect == 0) {
            return false;
        }
        m_visuals.placeAt(missile.effect, transformOf(missile),
                          missile.spec->spin == 0 ? std::optional{missile.velocity} : std::nullopt);
    }
    if (m_device != nullptr && launch.riderArchive != nullptr && !launch.riderTree.empty()) {
        EffectTrees::Setting setting;
        setting.persistent = true;
        setting.missingObjectsAreEmpty = true;
        const std::array<TextureSet*, 1> lenders{launch.textureLender};
        missile.rider = prepared ? m_visuals.startPrepared(*rider, missile.position, setting)
                                 : m_visuals.startSet(*m_device, *launch.riderArchive,
                                                      launch.riderTree, missile.position, setting,
                                                      launch.textureLender != nullptr
                                                          ? std::span{lenders}
                                                          : std::span<TextureSet* const>{});
        if (prepared && missile.rider == 0) {
            return false;
        }
        m_visuals.placeAt(missile.rider, transformOf(missile),
                          missile.spec->spin == 0 ? std::optional{missile.velocity} : std::nullopt);
    }
    std::optional<s32> character;
    if (launch.release && launch.release->character >= 0 && launch.release->character <= 16 &&
        launch.spec == &MissileSpec::of(launch.release->character)) {
        character = launch.release->character;
    } else if (!launch.release) {
        for (s32 candidate = 0; candidate <= 16; ++candidate) {
            if (launch.spec == &MissileSpec::of(candidate)) {
                character = candidate;
                break;
            }
        }
    }
    const bool potion = launch.potion >= 1 && launch.potion <= 4 &&
                        launch.spec == &MissileSpec::potion() && !launch.release &&
                        (launch.flags & ~damage::kHeal) == 0;
    const bool weapon =
        character && launch.potion == 0 && !launch.potionRelease && replayModifiers(launch.flags);
    if ((weapon || potion) && launch.multiplayer == MultiplayerMode::Normal) {
        ParticleFlight flight;
        flight.birth = flightCheckpoint(missile);
        if (flight.valid()) {
            m_sim.visualHistory.push_back({missile.instance, missile.owner, character.value_or(0),
                                           launch.release, missile.effect, missile.rider, flight,
                                           transformOf(missile), missile.velocity,
                                           potion ? launch.potion : 0, launch.potionRelease});
        }
    }
    m_sim.missiles.push_back(missile);
    return true;
}

void PlayerMissiles::bindVisuals(RenderDevice& device, std::span<TextureSet* const> lenders) {
    clear();
    m_device = &device;
    m_visuals.setTextureLenders(lenders);
}

f32 PlayerMissiles::damageFor(s32 stat) {
    return std::clamp(kLeastDamage +
                          kStatScale * static_cast<f32>(stat) * (kMostDamage - kLeastDamage),
                      kLeastDamage, kMostDamage);
}

f32 PlayerMissiles::hitGap(const ItemArchive* archive, std::string_view tree) {
    constexpr f32 kDefaultGap = 0.25f;
    constexpr f32 kEffectRate = 30.0f;
    if (archive != nullptr) {
        if (const auto index = archive->trees.find(tree)) {
            const auto& sequences = archive->trees.tree(*index).sequences;
            if (!sequences.empty() && sequences.front().frames > 0) {
                return static_cast<f32>(sequences.front().frames) / kEffectRate;
            }
        }
    }
    return kDefaultGap;
}

void PlayerMissiles::update(f32 seconds, const WorldCollision* collision,
                            std::span<const MissileTarget> targets,
                            std::span<const MissilePlayer> players) {
    m_visuals.capturePresentation();
    if (seconds > 0) {
        std::erase_if(m_sim.visualHistory, [seconds](const auto& history) {
            return !std::isfinite(seconds) || seconds < 1.0f / 240 || seconds > 0.25f ||
                   (history.flight.totalSteps != 0 && history.flight.stepSeconds != seconds);
        });
        for (auto& history : m_sim.visualHistory) {
            history.flight.stepSeconds = seconds;
            ++history.flight.totalSteps;
        }
    }
    m_sim.ricochetIn = std::max(0.0f, m_sim.ricochetIn - seconds);
    for (Missile& missile : m_sim.missiles) {
        missile.previousPosition = missile.position;
        missile.previousVelocity = missile.velocity;
        missile.previousTumble = missile.tumble;
        missile.previousAge = missile.age;
        missile.presentationCaptured = true;
        for (auto& contact : missile.playerContacts) {
            contact.remaining -= seconds;
        }
        const bool penetrates = missile.potion == 0 && (missile.flags & damage::kSuperShot) != 0;
        // Steps no longer than half its size, so no wall is flown clean through.
        const f32 radius = missile.spec->radius;
        const f32 travel = glm::length(missile.velocity) * seconds;
        const auto steps = std::max(1, static_cast<s32>(std::ceil(travel / (radius * 0.5f))));
        const f32 step = seconds / static_cast<f32>(steps);
        for (s32 i = 0; i < steps && missile.age < kLifeSeconds; ++i) {
            missile.velocity.y -= missile.spec->weight * step;
            missile.position += missile.velocity * step;
            missile.tumble += missile.spec->spin * step;
            missile.age += step;
            bool reflected = false;
            if (missile.potion == 0 && missile.multiplayer != MultiplayerMode::Normal) {
                for (const auto& player : players) {
                    if (player.player == missile.owner ||
                        !player.body.touches(missile.position, radius)) {
                        continue;
                    }
                    auto contact = std::ranges::find(missile.playerContacts, player.player,
                                                     &Missile::PlayerContact::player);
                    if (player.reflective) {
                        ++missile.continuity;
                        missile.presentationCaptured = false;
                        missile.velocity = -missile.velocity;
                        missile.position += missile.velocity * step;
                        missile.age += kReflectionTime;
                        missile.damage = std::min(missile.damage, kReflectedDamage);
                        MissileImpact impact;
                        impact.position = missile.position;
                        impact.owner = missile.owner;
                        impact.effect = {};
                        impact.wallSound = m_sim.ricochetIn <= 0 ? MissileWallSound::Ricochet
                                                                 : MissileWallSound::Silent;
                        m_sim.impacts.push_back(impact);
                        if (m_sim.ricochetIn <= 0) {
                            m_sim.ricochetIn = kReflectionTime;
                        }
                        reflected = true;
                        break;
                    }
                    if (penetrates && contact != missile.playerContacts.end() &&
                        contact->remaining > 0) {
                        continue;
                    }
                    MissileImpact impact;
                    impact.position = missile.position;
                    impact.owner = missile.owner;
                    impact.damage = missile.damage;
                    impact.flags = missile.flags;
                    impact.effect = missile.spec->impactTree;
                    if (penetrates) {
                        if ((missile.flags & damage::kReflect) != 0 &&
                            !missile.spec->impactTree.empty() && missile.damage > 2) {
                            // This update still passes through; the next contact update does not.
                            missile.flags &= ~damage::kSuperShot;
                            impact.position = player.body.base;
                        } else {
                            impact.effect = {};
                        }
                    }
                    impact.wallSound = MissileWallSound::Silent;
                    impact.player = player.player;
                    const f32 speed = glm::length(missile.velocity);
                    impact.direction = speed > 0 ? missile.velocity / speed : Vec3{0};
                    impact.stun = missile.multiplayer == MultiplayerMode::Stun;
                    impact.playerHitGap = penetrates ? kPlayerPassThroughGap : missile.playerHitGap;
                    m_sim.impacts.push_back(impact);
                    if (contact == missile.playerContacts.end()) {
                        missile.playerContacts.push_back({player.player, impact.playerHitGap});
                    } else {
                        contact->remaining = impact.playerHitGap;
                    }
                    if (!penetrates) {
                        missile.age = kLifeSeconds;
                    }
                    break;
                }
            }
            if (reflected) {
                continue;
            }
            if (missile.age >= kLifeSeconds) {
                break;
            }
            // What stands in its way stops it before any wall behind does.
            for (const auto& target : targets) {
                if (target.potionBottle && !missile.breaksPotions) {
                    continue;
                }
                if (std::ranges::find(missile.pierced, target.id) != missile.pierced.end()) {
                    continue;
                }
                const bool contact = target.touches(missile.position, radius);
                if (!contact) {
                    continue;
                }
                MissileImpact impact{
                    missile.position, missile.owner, missile.potion,           missile.potency,
                    missile.damage,   target.id,     missile.spec->impactTree, missile.wallSound,
                    missile.flags,    target.node};
                // ProcessEffects normalizes the missile's flight velocity for
                // CritterDamage. The owner may have moved since releasing it.
                const f32 speed = glm::length(missile.velocity);
                impact.direction = speed > 0 ? missile.velocity / speed : Vec3{0};
                m_sim.impacts.push_back(impact);
                missile.pierced.push_back(target.id);
                if (!penetrates) {
                    missile.age = kLifeSeconds;
                    break;
                }
            }
            if (missile.age >= kLifeSeconds) {
                break;
            }
            if (collision == nullptr || penetrates) {
                continue;
            }
            std::vector<WallContact> contacts;
            // WeaponWallCollide: the fences only the party walks against let shots by.
            const Vec3 pushed = collision->resolveWalls(
                missile.position, radius, missile.position.y - radius * 0.5f,
                missile.position.y + radius * 0.5f, &contacts, std::nullopt,
                WorldCollision::WallPush::All, WorldCollision::kMissileQueryFlags);
            const bool wall = glm::distance(pushed, missile.position) > 1e-4f;
            const auto floor =
                collision->projectileFloorAt(missile.position, radius, radius * 0.5f);
            if (wall || floor.has_value()) {
                s32 object = floor ? floor->object : -1;
                if (wall && !contacts.empty()) {
                    object = contacts.front().object;
                }
                if (missile.potion == 0 && (missile.flags & damage::kReflect) != 0) {
                    ++missile.continuity;
                    missile.presentationCaptured = false;
                    const Vec3 normal =
                        wall ? glm::normalize(pushed - missile.position) : floor->normal;
                    missile.position = pushed;
                    if (!wall) {
                        missile.position.y = floor->y + radius + 0.01f;
                    }
                    if (glm::dot(missile.velocity, normal) < 0) {
                        missile.velocity = glm::reflect(missile.velocity, normal);
                        if (missile.velocity.y > 0) {
                            missile.velocity.y *= 0.4f;
                        }
                        missile.age += 1.0f;
                        m_sim.impacts.push_back({missile.position,
                                                 missile.owner,
                                                 0,
                                                 0,
                                                 0,
                                                 -1,
                                                 {},
                                                 m_sim.ricochetIn <= 0 ? MissileWallSound::Ricochet
                                                                       : MissileWallSound::Silent,
                                                 missile.flags,
                                                 -1,
                                                 object});
                        if (m_sim.ricochetIn <= 0) {
                            m_sim.ricochetIn = 1.0f;
                        }
                    }
                    continue;
                }
                m_sim.impacts.push_back(MissileImpact{
                    missile.position, missile.owner, missile.potion, missile.potency,
                    missile.damage, -1, missile.spec->impactTree, missile.wallSound, missile.flags,
                    -1, object,
                    !wall && floor && (floor->objectFlags & WorldCollision::kLiquidSurface) != 0});
                missile.age = kLifeSeconds;
            }
        }
    }
    for (const Missile& missile : m_sim.missiles) {
        m_visuals.placeAt(missile.effect, transformOf(missile),
                          missile.spec->spin == 0 ? std::optional{missile.velocity} : std::nullopt);
        m_visuals.placeAt(missile.rider, transformOf(missile),
                          missile.spec->spin == 0 ? std::optional{missile.velocity} : std::nullopt);
        if (!missile.presentationCaptured) {
            m_visuals.snapPresentation(missile.effect);
            m_visuals.snapPresentation(missile.rider);
        }
        if (missile.age >= kLifeSeconds) {
            m_visuals.finish(missile.effect);
            m_visuals.finish(missile.rider);
        }
        if (seconds > 0) {
            const auto history =
                std::ranges::find(m_sim.visualHistory, missile.instance, &VisualHistory::instance);
            if (history != m_sim.visualHistory.end()) {
                history->flight.retiring = missile.age >= kLifeSeconds;
                history->flight.activeSteps += history->flight.retiring ? 0 : 1;
                history->placement = transformOf(missile);
                history->direction = missile.velocity;
            }
        }
    }
    m_visuals.update(seconds);
    std::erase_if(m_sim.missiles, [](const Missile& m) { return m.age >= kLifeSeconds; });
    std::erase_if(m_sim.visualHistory, [&](const auto& history) {
        return !history.flight.valid() ||
               (m_device != nullptr && !m_visuals.playing(history.effect) &&
                !m_visuals.playing(history.rider));
    });
}

Mat4 PlayerMissiles::transformOf(const Missile& missile, f32 frameBlend) {
    const f32 blend =
        missile.presentationCaptured && frameBlend >= 0 ? std::clamp(frameBlend, 0.0f, 1.0f) : 1.0f;
    const Vec3 velocity = glm::mix(missile.previousVelocity, missile.velocity, blend);
    const Vec3 position = glm::mix(missile.previousPosition, missile.position, blend);
    const f32 tumble = glm::mix(missile.previousTumble, missile.tumble, blend);
    const f32 yaw = portable::atan2(velocity.x, velocity.z);
    const f32 level = portable::hypot(velocity.x, velocity.z);
    // About x, a positive turn tips the nose down: a climbing missile noses up by its climb,
    // and a tumbling one goes on over forwards.
    const f32 pitch = -portable::atan2(velocity.y, level);
    Mat4 out = glm::translate(Mat4{1.0f}, position);
    out = portable::rotate(out, yaw, Vec3{0.0f, 1.0f, 0.0f});
    out =
        portable::rotate(out, missile.spec->spin != 0.0f ? tumble : pitch, Vec3{1.0f, 0.0f, 0.0f});
    return glm::scale(out, Vec3{missile.scale, missile.scale, missile.scale});
}

void PlayerMissiles::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                          const CameraFrame* camera, f32 frameBlend) const {
    for (const Missile& missile : m_sim.missiles) {
        if (missile.effect == 0 && missile.model != nullptr && missile.model->bound()) {
            Mat4 placement = transformOf(missile, frameBlend);
            if (camera != nullptr && missile.spec->spin == 0) {
                const f32 blend = missile.presentationCaptured && frameBlend >= 0
                                      ? std::clamp(frameBlend, 0.0f, 1.0f)
                                      : 1.0f;
                placement = camera->along(
                    placement, glm::mix(missile.previousVelocity, missile.velocity, blend));
            }
            missile.model->draw(device, clip, placement, lighting, {}, camera);
        }
    }
    m_visuals.draw(device, clip, lighting, camera, frameBlend);
    if (camera != nullptr) {
        for (const Missile& missile : m_sim.missiles) {
            if (missile.streak.texture == nullptr) {
                continue;
            }
            const f32 blend = missile.presentationCaptured && frameBlend >= 0
                                  ? std::clamp(frameBlend, 0.0f, 1.0f)
                                  : 1.0f;
            const auto batch = missile.streak.geometry(
                glm::mix(missile.previousPosition, missile.position, blend),
                glm::mix(missile.previousVelocity, missile.velocity, blend),
                glm::mix(missile.previousAge, missile.age, blend), missile.spec->radius, *camera);
            // MBPolyInst: alpha blend, alpha compare > 2, depth test and depth write.
            DrawState state;
            state.alphaTest = DrawState::kTranslucentAlphaTest;
            device.draw(batch, *missile.streak.texture, clip, state);
        }
    }
}

void PlayerMissiles::clear() {
    ++m_restoreGeneration;
    ++m_localEpoch;
    m_sim.visualHistory.clear();
    m_sim.ricochetIn = 0;
    m_sim.missiles.clear();
    m_sim.impacts.clear();
    m_visuals.clear();
    m_device = nullptr;
}

std::vector<MissileImpact> PlayerMissiles::takeImpacts() {
    return std::exchange(m_sim.impacts, {});
}

void PlayerMissiles::saveLocal(LocalSnapshot& state) const {
    state.m_owner.reset();
    state.m_state = m_sim;
    m_visuals.saveLocal(state.m_visuals);
    state.m_device = m_device;
    state.m_epoch = m_localEpoch;
    state.m_owner = m_restoreOwner;
}

bool PlayerMissiles::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("PlayerMissiles.native.1");
    digest.flag(m_device != nullptr);
    digest.value(m_sim);
    return m_visuals.appendNativeDigest(digest) && digest.good();
}

bool PlayerMissiles::appendLocalDigest(NativeStateDigest& digest,
                                       const LocalSnapshot& state) const {
    if (!canRestoreLocal(state)) {
        digest.invalidate();
        return false;
    }
    digest.string("PlayerMissiles.native.1");
    digest.flag(state.m_device != nullptr);
    digest.value(state.m_state);
    return m_visuals.appendLocalDigest(digest, state.m_visuals) && digest.good();
}

bool PlayerMissiles::registerNativeResources(NativeStateResources& resources) const {
    return m_visuals.registerNativeResources(resources);
}

bool PlayerMissiles::canRestoreLocal(const LocalSnapshot& state) const {
    return !state.m_owner.expired() && state.m_owner.lock() == m_restoreOwner &&
           state.m_epoch == m_localEpoch && state.m_device == m_device &&
           m_visuals.canRestoreLocal(state.m_visuals);
}

bool PlayerMissiles::restoreLocal(const LocalSnapshot& state) {
    if (!canRestoreLocal(state)) {
        return false;
    }
    m_sim = state.m_state;
    (void)m_visuals.restoreLocal(state.m_visuals);
    ++m_restoreGeneration;
    return true;
}

std::vector<Vec3> PlayerMissiles::spread(const Vec3& direction, s32 shots) {
    // The original's order: straight on first, then a pair to each side, the nearer first.
    constexpr std::array<f32, 5> kTurns{0.0f, 1.0f, -1.0f, 2.0f, -2.0f};
    std::vector<Vec3> out;
    for (s32 i = 0; i < std::clamp(shots, 1, static_cast<s32>(kTurns.size())); ++i) {
        const f32 angle = kTurns[static_cast<usize>(i)] * kSpreadStep;
        const f32 c = portable::cos(angle);
        const f32 s = portable::sin(angle);
        out.emplace_back(direction.x * c + direction.z * s, direction.y,
                         -direction.x * s + direction.z * c);
    }
    return out;
}

} // namespace gdl::game
