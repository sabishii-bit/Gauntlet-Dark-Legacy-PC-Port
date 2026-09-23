#include "game/world/PlayerMissiles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <utility>

namespace gdl::game {

namespace {

constexpr float kTumble = 18.85f; ///< three turns a second
constexpr std::string_view kCostumeTiers = "0000000000";
constexpr std::string_view kStaffTiers = "1112223333";
constexpr std::string_view kBombTiers = "1111112233";
constexpr std::string_view kFirstTiers = "1111111111";
constexpr std::size_t kFamilyCount = 8;
constexpr int kSumner = 16;
constexpr int kWizard = 2;

/** The sixteen classes' throws: the eight to start with, then the eight that shadow them. */
constexpr std::array<MissileSpec, 16> kSpecs{{
    {"AXE", kCostumeTiers, 1.0f, kTumble, 12.0f, false},
    {"SWD", kCostumeTiers, 1.0f, kTumble, 8.0f, false},
    {"STF", kStaffTiers, 1.2f, 0.0f, 8.0f, true},
    {"BOW", kStaffTiers, 0.7f, 0.0f, 8.0f, true},
    {"HAM", kCostumeTiers, 1.0f, kTumble, 20.0f, false},
    {"MAC", kCostumeTiers, 1.0f, kTumble, 8.0f, false},
    {"WND", kStaffTiers, 1.2f, 0.0f, 8.0f, true},
    {"BOM", kBombTiers, 0.7f, 0.0f, 8.0f, false},
    {"MIN", kFirstTiers, 1.0f, kTumble, 12.0f, false},
    {"FAL", kFirstTiers, 1.0f, kTumble, 8.0f, false},
    {"STF", kStaffTiers, 1.2f, 0.0f, 8.0f, true},
    {"BOW", kStaffTiers, 0.7f, 0.0f, 8.0f, true},
    {"OGR", kFirstTiers, 1.0f, kTumble, 20.0f, false},
    {"UNI", kFirstTiers, 1.0f, kTumble, 8.0f, false},
    {"WND", kStaffTiers, 1.2f, 0.0f, 8.0f, true},
    {"BOM", kBombTiers, 0.7f, 0.0f, 8.0f, false},
}};

std::size_t specIndex(int classIndex) {
    if (classIndex == kSumner) {
        return kWizard;
    }
    return static_cast<std::size_t>(std::clamp(classIndex, 0, static_cast<int>(kSpecs.size()) - 1));
}

} // namespace

const MissileSpec& MissileSpec::of(int classIndex) {
    return kSpecs[specIndex(classIndex)];
}

std::string MissileSpec::treeName(int classIndex, int level, bool* inCostume) {
    const MissileSpec& spec = of(classIndex);
    const auto tier = static_cast<std::size_t>(std::clamp(level / 10, 0, 9));
    const char mark = spec.tiers[tier];
    if (inCostume != nullptr) {
        *inCostume = mark == '0';
    }
    return std::format("{}_THROW{}", spec.model, mark);
}

const MissileSpec& MissileSpec::potion() {
    static constexpr MissileSpec kPotion{"POT", kCostumeTiers, 0.7f, kTumble, 12.0f, false};
    return kPotion;
}

bool MissileSpec::byMagic(int classIndex) {
    const std::size_t family = specIndex(classIndex) % kFamilyCount;
    return family == 2 || family == 6;
}

float PlayerMissiles::speedFor(int stat) {
    return kSlowest +
           kStatScale * static_cast<float>(std::clamp(stat, 0, 1000)) * (kFastest - kSlowest);
}

float PlayerMissiles::reachFor(float attackSeconds) {
    return kReach + kReachPerSecond * std::clamp(attackSeconds - kHoldDelay, 0.0f, kHoldMost);
}

Vec3 PlayerMissiles::launchVelocity(const Vec3& direction, float speed, float reach, float weight) {
    // Over the time the reach takes, gravity is cancelled and the drop made up.
    const float flight = reach / speed;
    return Vec3{direction.x * speed, 0.5f * weight * flight - kDrop / flight, direction.z * speed};
}

bool PlayerMissiles::launch(const MissileLaunch& launch) {
    if (launch.spec == nullptr || launch.speed <= 0.0f || launch.reach <= 0.0f) {
        return false;
    }
    Missile missile;
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
    m_missiles.push_back(missile);
    return true;
}

float PlayerMissiles::damageFor(int stat) {
    return std::clamp(kLeastDamage +
                          kStatScale * static_cast<float>(stat) * (kMostDamage - kLeastDamage),
                      kLeastDamage, kMostDamage);
}

void PlayerMissiles::update(float seconds, const WorldCollision* collision,
                            std::span<const MissileTarget> targets) {
    for (Missile& missile : m_missiles) {
        // Steps no longer than half its size, so no wall is flown clean through.
        const float radius = missile.spec->radius;
        const float travel = glm::length(missile.velocity) * seconds;
        const auto steps = std::max(1, static_cast<int>(std::ceil(travel / (radius * 0.5f))));
        const float step = seconds / static_cast<float>(steps);
        for (int i = 0; i < steps && missile.age < kLifeSeconds; ++i) {
            missile.velocity.y -= missile.spec->weight * step;
            missile.position += missile.velocity * step;
            missile.tumble += missile.spec->spin * step;
            missile.age += step;
            // What stands in its way stops it before any wall behind does.
            const auto struck = std::ranges::find_if(targets, [&](const MissileTarget& target) {
                const float reach = target.radius + radius;
                return std::hypot(missile.position.x - target.base.x,
                                  missile.position.z - target.base.z) <= reach &&
                       missile.position.y + radius >= target.base.y &&
                       missile.position.y - radius <= target.base.y + target.height;
            });
            if (struck != targets.end()) {
                m_impacts.push_back(MissileImpact{missile.position, missile.owner, missile.potion,
                                                  missile.potency, missile.damage, struck->id});
                missile.age = kLifeSeconds;
                break;
            }
            if (collision == nullptr) {
                continue;
            }
            const Vec3 pushed = collision->resolveWalls(missile.position, radius,
                                                        missile.position.y - radius * 0.5f,
                                                        missile.position.y + radius * 0.5f);
            const bool wall = glm::distance(pushed, missile.position) > 1e-4f;
            const bool floor =
                collision->floorAt(missile.position, radius, radius * 0.5f).has_value();
            if (wall || floor) {
                m_impacts.push_back(MissileImpact{missile.position, missile.owner, missile.potion,
                                                  missile.potency, missile.damage, -1});
                missile.age = kLifeSeconds;
            }
        }
    }
    std::erase_if(m_missiles, [](const Missile& m) { return m.age >= kLifeSeconds; });
}

Mat4 PlayerMissiles::transformOf(const Missile& missile) {
    const float yaw = std::atan2(missile.velocity.x, missile.velocity.z);
    const float level = std::hypot(missile.velocity.x, missile.velocity.z);
    // About x, a positive turn tips the nose down: a climbing missile noses up by its climb,
    // and a tumbling one goes on over forwards.
    const float pitch = -std::atan2(missile.velocity.y, level);
    Mat4 out = glm::translate(Mat4{1.0f}, missile.position);
    out = glm::rotate(out, yaw, Vec3{0.0f, 1.0f, 0.0f});
    out = glm::rotate(out, missile.spec->spin != 0.0f ? missile.tumble : pitch,
                      Vec3{1.0f, 0.0f, 0.0f});
    return glm::scale(out, Vec3{missile.scale, missile.scale, missile.scale});
}

void PlayerMissiles::draw(RenderDevice& device, const Mat4& clip,
                          const WorldLighting& lighting) const {
    for (const Missile& missile : m_missiles) {
        if (missile.model != nullptr && missile.model->bound()) {
            missile.model->draw(device, clip, transformOf(missile), lighting);
        }
    }
}

void PlayerMissiles::clear() {
    m_missiles.clear();
    m_impacts.clear();
}

std::vector<MissileImpact> PlayerMissiles::takeImpacts() {
    return std::exchange(m_impacts, {});
}

std::vector<Vec3> PlayerMissiles::spread(const Vec3& direction, int shots) {
    // The original's order: straight on first, then a pair to each side, the nearer first.
    constexpr std::array<float, 5> kTurns{0.0f, 1.0f, -1.0f, 2.0f, -2.0f};
    std::vector<Vec3> out;
    for (int i = 0; i < std::clamp(shots, 1, static_cast<int>(kTurns.size())); ++i) {
        const float angle = kTurns[static_cast<std::size_t>(i)] * kSpreadStep;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        out.emplace_back(direction.x * c + direction.z * s, direction.y,
                         -direction.x * s + direction.z * c);
    }
    return out;
}

} // namespace gdl::game
