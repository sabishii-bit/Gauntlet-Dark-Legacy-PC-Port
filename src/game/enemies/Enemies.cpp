#include "game/enemies/Enemies.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "engine/core/Types.h"

#include "game/combat/Damage.h"
#include "game/enemies/EnemyMissiles.h"

namespace gdl::game {

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr f32 kStepUp = 1.5f;
constexpr f32 kDrop = 3.0f;
constexpr f32 kFootClearance = 0.1f;
constexpr f32 kSpawnDrop = 6.0f;       ///< a spawn finds its floor within this
constexpr s32 kFarRecycleCost = 10000; ///< an unseen enemy is that much cheaper to reuse
constexpr f32 kPushFloor = 0.01f;
constexpr f32 kGravity = 100.0f;
constexpr f32 kDeathSkinRate = 15.0f;

constexpr s32 kRetargetEvery = 8; ///< frames between a mind looking round again
constexpr f32 kRunFrom = 1.25f;   ///< a pace this much over a walk's runs
constexpr f32 kStopped = 0.01f;   ///< a step that gets less than this is a dead stop

// The octants about a generator, as its facing is turned into each.
Vec3 octant(const Vec3& v, s32 direction, f32& yawOffset) {
    constexpr f32 kHalfRoot = 0.707f;
    const f32 x = v.x;
    const f32 z = v.z;
    switch (direction) {
    case 1: yawOffset = kPi; return Vec3{-x, v.y, -z};
    case 2: yawOffset = -kPi / 2.0f; return Vec3{-z, v.y, x};
    case 3: yawOffset = kPi / 2.0f; return Vec3{z, v.y, -x};
    case 4:
        yawOffset = kPi / 4.0f;
        return Vec3{kHalfRoot * x + kHalfRoot * z, v.y, kHalfRoot * -x + kHalfRoot * z};
    case 5:
        yawOffset = -kPi / 4.0f;
        return Vec3{kHalfRoot * -z + kHalfRoot * x, v.y, kHalfRoot * z + kHalfRoot * x};
    case 6:
        yawOffset = 3.0f * kPi / 4.0f;
        return Vec3{kHalfRoot * z - kHalfRoot * x, v.y, kHalfRoot * -x - kHalfRoot * z};
    case 7:
        yawOffset = -3.0f * kPi / 4.0f;
        return Vec3{kHalfRoot * -z - kHalfRoot * x, v.y, kHalfRoot * z - kHalfRoot * x};
    default: yawOffset = 0.0f; return v;
    }
}

// The octants a kind may be born into: the humanoids only ahead of the generator.
u32 octantMaskOf(s32 kind) {
    switch (kind) {
    case 1:
    case 4:
    case 5:
    case 7:
    case 8:
    case 10:
    case 11:
    case 14:
    case 15:
    case 19:
    case 24:
    case 25: return 0xFFCE;
    default: return 0;
    }
}

// The small kinds (scorpions, rats, snakes, spiders, maggots, wolves, dogs, acid, hands) go
// about the way of their own, whatever a generator asks.
bool smallKind(s32 kind) {
    switch (kind) {
    case 0:
    case 3:
    case 6:
    case 9:
    case 12:
    case 15:
    case 18:
    case 21:
    case 22: return true;
    default: return false;
    }
}

// Which way to turn round something at `to`: along the axis of the wider gap.
s32 turnDirection(const Vec3& from, const Vec3& to) {
    if (std::abs(from.x - to.x) >= std::abs(from.z - to.z)) {
        return from.z < to.z ? 1 : -1;
    }
    return from.x < to.x ? -1 : 1;
}

f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

Enemies::~Enemies() {
    close();
}

void Enemies::open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                   const WorldCollision* collision, s32 most, const EnemyScales& scales, u32 seed) {
    close();
    m_device = &device;
    m_root = unpackedRoot;
    m_collision = collision;
    m_most = std::clamp(most, 1, kMost);
    m_scales = scales;
    m_random.seed(seed);
}

void Enemies::close() {
    for (Enemy& enemy : m_enemies) {
        enemy = Enemy{};
    }
    for (auto& stock : m_stocks) {
        for (TreeModel& body : stock->bodies) {
            body.clear();
        }
        for (TreeModel& body : stock->variantBodies) {
            body.clear();
        }
        stock->arrow.clear();
        stock->bomb.clear();
        stock->archive.release();
    }
    m_stocks.clear();
    m_blows.clear();
    m_losses.clear();
    m_feedback.clear();
    m_device = nullptr;
    m_collision = nullptr;
    m_frame = 0;
}

Enemies::Stock* Enemies::stockOf(s32 kind) {
    for (auto& stock : m_stocks) {
        if (stock->kind == kind) {
            return stock.get();
        }
    }
    return nullptr;
}

const Enemies::Stock* Enemies::stockOf(s32 kind) const {
    for (const auto& stock : m_stocks) {
        if (stock->kind == kind) {
            return stock.get();
        }
    }
    return nullptr;
}

bool Enemies::loadKind(s32 kind) {
    if (stockOf(kind) != nullptr) {
        return true;
    }
    if (m_device == nullptr || kind < 0 || kind >= kEnemyKindCount) {
        return false;
    }
    const EnemyKind& info = enemyKind(kind);
    auto stock = std::make_unique<Stock>();
    stock->kind = kind;
    if (!stock->archive.load(m_root / "MONSTERS" / std::string(info.name))) {
        return false;
    }
    for (s32 tier = 1; tier <= 3; ++tier) {
        const auto tree = stock->archive.trees.find(std::format("{}{}", info.prefix, tier));
        if (!tree.has_value()) {
            continue;
        }
        const TreeInfo& treeInfo = stock->archive.trees.tree(*tree);
        if (stock->bodies[static_cast<usize>(tier - 1)].bind(treeInfo, stock->archive.models,
                                                             stock->archive.textures, *m_device)) {
            stock->trees[static_cast<usize>(tier - 1)] = &treeInfo;
        }
    }
    // The archer, bomber and suicide ("ZOMA", "ZOMB", "ZOMS") and what the first two throw.
    const std::array<const char*, 3> suffixes{"A", "B", "S"};
    for (usize v = 0; v < suffixes.size(); ++v) {
        const auto tree = stock->archive.trees.find(std::format("{}{}", info.prefix, suffixes[v]));
        if (!tree.has_value()) {
            continue;
        }
        const TreeInfo& treeInfo = stock->archive.trees.tree(*tree);
        if (stock->variantBodies[v].bind(treeInfo, stock->archive.models, stock->archive.textures,
                                         *m_device)) {
            stock->variantTrees[v] = &treeInfo;
        }
    }
    if (const auto arrow = stock->archive.trees.find(std::format("{}_ARROW", info.prefix));
        arrow.has_value()) {
        stock->arrow.bind(stock->archive.trees.tree(*arrow), stock->archive.models,
                          stock->archive.textures, *m_device);
    }
    if (const auto bomb = stock->archive.trees.find(std::format("{}_BOMB", info.prefix));
        bomb.has_value()) {
        stock->bomb.bind(stock->archive.trees.tree(*bomb), stock->archive.models,
                         stock->archive.textures, *m_device);
    }
    m_stocks.push_back(std::move(stock));
    return true;
}

bool Enemies::kindLoaded(s32 kind) const {
    return stockOf(kind) != nullptr;
}

const ItemArchive* Enemies::archiveOf(s32 kind) const {
    const Stock* stock = stockOf(kind);
    return stock != nullptr ? &stock->archive : nullptr;
}

ItemArchive* Enemies::archive(s32 kind) {
    Stock* stock = stockOf(kind);
    return stock != nullptr ? &stock->archive : nullptr;
}

const TreeInfo* Enemies::treeOf(s32 kind, s32 tier) const {
    const Stock* stock = stockOf(kind);
    if (stock == nullptr) {
        return nullptr;
    }
    // A tier the archive lacks wears the nearest it has.
    for (s32 t = std::clamp(tier, 1, 3); t >= 1; --t) {
        if (const TreeInfo* tree = stock->trees[static_cast<usize>(t - 1)]; tree != nullptr) {
            return tree;
        }
    }
    for (const TreeInfo* tree : stock->trees) {
        if (tree != nullptr) {
            return tree;
        }
    }
    return nullptr;
}

f32 Enemies::paceOf(s32 kind) const {
    return enemyKind(kind).pace * m_scales.speed;
}

Vec3 Enemies::bodyCentre(const Enemy& enemy) {
    return enemy.position + Vec3{0.0f, enemy.reach, 0.0f};
}

const EnemyView* Enemies::viewOf(std::span<const EnemyView> players, s32 player) {
    for (const EnemyView& view : players) {
        if (view.player == player) {
            return &view;
        }
    }
    return nullptr;
}

// ---- spawning ---------------------------------------------------------------------------

std::optional<s32> Enemies::takeSlot(const EnemySpawn& spawn, std::span<const EnemyView> players) {
    // Prefer a free slot, otherwise the greatest recycling score. Dying/sleeping
    // enemies get reduced scores; other unseen enemies get the offscreen bonus.
    for (s32 i = 0; i < m_most; ++i) {
        if (m_enemies[static_cast<usize>(i)].state == State::Inactive) {
            return i;
        }
    }
    s32 best = -1;
    bool bestVisible = false;
    f32 bestCost = -1.0f;
    for (s32 i = 0; i < m_most; ++i) {
        const Enemy& enemy = m_enemies[static_cast<usize>(i)];
        if (enemy.kind == kItKind) {
            continue;
        }
        f32 cost = enemy.targetDistance;
        bool seen = false;
        for (const EnemyView& view : players) {
            seen = seen ||
                   (!view.hidden && flatDistance(view.position, enemy.position) <= enemy.sight);
        }
        if (enemy.state == State::Dying || enemy.state == State::Asleep) {
            cost *= 0.01f;
        } else if (!seen) {
            cost += kFarRecycleCost;
        }
        if (cost > bestCost) {
            bestCost = cost;
            best = i;
            bestVisible = seen;
        }
    }
    if (best < 0) {
        return std::nullopt;
    }
    // Visibility importance, not combat strength, controls replacement. Visibility still
    // uses the population's proximity approximation until a camera-frustum query is supplied.
    if (spawn.kind < kSwarmKindCount && static_cast<s32>(spawn.priority) < (bestVisible ? 1 : 0)) {
        return std::nullopt;
    }
    Enemy& taken = m_enemies[static_cast<usize>(best)];
    taken = Enemy{};
    return best;
}

bool Enemies::clearAt(Enemy& enemy, const Vec3& position, std::span<const EnemyView> players,
                      std::span<const Obstacle> obstacles, s32 self) const {
    for (const EnemyView& view : players) {
        if (flatDistance(view.position, position) < view.radius + enemy.radius &&
            std::abs(view.position.y - position.y) < view.height) {
            return false;
        }
    }
    for (s32 i = 0; i < m_most; ++i) {
        const Enemy& other = m_enemies[static_cast<usize>(i)];
        if (i == self || other.state == State::Inactive) {
            continue;
        }
        if (flatDistance(other.position, position) < 0.5f * enemy.radius + other.radius &&
            std::abs(other.position.y - position.y) < other.height) {
            return false;
        }
    }
    return std::ranges::none_of(obstacles, [&](const Obstacle& box) {
        return box.solid && box.pushOut(position, enemy.radius) != position;
    });
}

void Enemies::initialise(Enemy& enemy, const EnemySpawn& spawn, const EnemyKind& kind) {
    enemy.state = spawn.asleep ? State::Asleep : State::Active;
    enemy.kind = spawn.kind;
    // A strength past the tiers is a variant: the archer and bomber of the second tier, the
    // suicide of the first, each with its own way unless the placement gives one.
    enemy.variant = spawn.tier >= kArcherStrength ? spawn.tier : 0;
    if (enemy.variant == 0) {
        enemy.tier = std::clamp(spawn.tier, 1, 3);
    } else {
        enemy.tier = enemy.variant == kSuicideStrength ? 1 : 2;
    }
    enemy.idleTicks = spawn.idleTicks;
    enemy.algorithm =
        spawn.algorithm >= 0 && spawn.algorithm < 32 ? spawn.algorithm : kind.algorithm;
    if (spawn.algorithm < 0 || spawn.algorithm == kind.algorithm) {
        switch (enemy.variant) {
        case kArcherStrength: enemy.algorithm = kSkirmishWay; break;
        case kBomberStrength: enemy.algorithm = kBombWay; break;
        case kSuicideStrength: enemy.algorithm = kSuicideWay; break;
        default: break;
        }
    }
    if (smallKind(spawn.kind) && enemy.algorithm != 2 && enemy.algorithm != 4) {
        enemy.algorithm = 2;
    }
    enemy.generator = spawn.generator;
    enemy.radius = kind.radius;
    enemy.height = kind.height;
    enemy.reach = 0.5f * kind.height;
    enemy.sight = kBaseSight * m_scales.sight;
    // Death alone is not scaled by the level; the swarm has a third of its kind's health a
    // tier, and hits softer as it loses its kind's full share.
    const f32 scale = spawn.kind != kDeathKind ? m_scales.health : 1.0f;
    enemy.fullHealth = kind.health * scale;
    enemy.health =
        spawn.kind < kSwarmKindCount ? kind.healthAtTier(enemy.tier) * scale : enemy.fullHealth;
    enemy.target = -1;
    enemy.targetBefore = -1;
    enemy.targetDistance = 100000.0f;
    enemy.weightedDistance = 100000.0f;
    enemy.contact = -1;
    enemy.attackIndex = -1;
}

std::optional<s32> Enemies::spawn(const EnemySpawn& spawn, std::span<const EnemyView> players,
                                  std::span<const Obstacle> obstacles) {
    const Stock* stock = stockOf(spawn.kind);
    if (stock == nullptr) {
        return std::nullopt;
    }
    const auto slot = takeSlot(spawn, players);
    if (!slot.has_value()) {
        return std::nullopt;
    }
    Enemy& enemy = m_enemies[static_cast<usize>(*slot)];
    const EnemyKind& kind = enemyKind(spawn.kind);
    initialise(enemy, spawn, kind);
    const TreeInfo* tree = treeOf(spawn.kind, enemy.tier);
    if (enemy.variant != 0 &&
        stock->variantTrees[static_cast<usize>(enemy.variant - kArcherStrength)] != nullptr) {
        tree = stock->variantTrees[static_cast<usize>(enemy.variant - kArcherStrength)];
    }
    const bool walksIn = enemy.variant == 0 && (spawn.kind == 1 || spawn.kind == kGruntKind ||
                                                spawn.kind == 10 || spawn.kind == 7);
    if (tree == nullptr || !enemy.animator.bind(*tree, walksIn)) {
        enemy = Enemy{};
        return std::nullopt;
    }
    const f32 facing = std::atan2(spawn.direction.x, spawn.direction.z);
    const auto settle = [this](Vec3 at, Vec3& out) {
        if (m_collision == nullptr) {
            out = at;
            return true;
        }
        const auto floor = m_collision->floorAt(at, kSpawnDrop, kSpawnDrop);
        if (!floor.has_value()) {
            return false;
        }
        at.y = floor->y;
        out = at;
        return true;
    };
    bool placed = false;
    Vec3 where = spawn.position;
    f32 yaw = facing;
    if (spawn.placed) {
        placed = settle(spawn.position, where);
    } else {
        const f32 out = spawn.clearance + enemy.radius;
        const Vec3 v = spawn.direction * out;
        u32 mask = octantMaskOf(spawn.kind);
        const s32 directions = 8;
        const auto start = static_cast<s32>(m_random() % static_cast<u32>(directions));
        s32 d = start;
        do {
            if ((mask & (1U << static_cast<u32>(d))) == 0) {
                f32 yawOffset = 0.0f;
                const Vec3 offset = octant(v, d, yawOffset);
                Vec3 at = spawn.position + offset;
                bool clear = settle(at, at) && std::abs(at.y - spawn.position.y) <= kSpawnDrop;
                if (clear && m_collision != nullptr) {
                    const Vec3 pushed =
                        m_collision->resolveWalls(at, enemy.radius, at.y + kFootClearance,
                                                  at.y + enemy.height - kFootClearance);
                    clear = flatDistance(pushed, at) < 0.01f;
                }
                if (!clear) {
                    mask |= 1U << static_cast<u32>(d);
                } else if (clearAt(enemy, at, players, obstacles, *slot)) {
                    where = at;
                    yaw = wrapAngle(facing + yawOffset);
                    placed = true;
                    break;
                }
            }
            d = (d + 1) % directions;
        } while (d != start);
    }
    if (!placed) {
        enemy = Enemy{};
        return std::nullopt;
    }
    enemy.position = where;
    enemy.yaw = yaw;
    enemy.mind.heading = yaw;
    enemy.mind.headingBefore = yaw;
    return slot;
}

void Enemies::wake(s32 id) {
    if (id >= 0 && id < m_most && m_enemies[static_cast<usize>(id)].state == State::Asleep) {
        m_enemies[static_cast<usize>(id)].state = State::Active;
    }
}

void Enemies::generatorGone(s32 generator) {
    for (Enemy& enemy : m_enemies) {
        if (enemy.generator == generator) {
            enemy.generator = -1;
        }
    }
}

// ---- the tick ----------------------------------------------------------------------------

void Enemies::update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                     std::span<const Obstacle> obstacles, EnemyMissiles* missiles,
                     f32 missileSpeedScale) {
    if (ticks <= 0) {
        return;
    }
    ++m_frame;
    std::array<f32, 4> crowding{};
    for (s32 i = 0; i < m_most; ++i) {
        Enemy& enemy = m_enemies[static_cast<usize>(i)];
        if (enemy.state == State::Inactive || enemy.state == State::Asleep) {
            continue;
        }
        enemy.flashSeconds = std::max(0.0f, enemy.flashSeconds - seconds);
        if (enemy.state == State::Dying) {
            enemy.deathSeconds += seconds;
            react(enemy);
            enemy.animator.request(EnemyAction::Dying);
            enemy.yaw = turnToward(enemy, enemy.mind.heading, ticks);
            move(enemy, i, ticks, seconds, Vec3{0.0f, 0.0f, 0.0f}, players, obstacles);
            enemy.animator.update(ticks, seconds, false);
            // A completed dissolve retires the body even if its fall is still playing.
            const bool dissolved =
                !enemy.deathSkin.empty() &&
                enemy.deathSeconds * kDeathSkinRate >= static_cast<f32>(enemy.deathSkinFrames);
            if (dissolved || enemy.animator.dead() || !enemy.animator.reacting()) {
                die(enemy);
            }
            continue;
        }
        chooseTarget(enemy, i, players, crowding);
        resolveBlows(enemy, i, players);
        react(enemy);
        if (enemy.state == State::Dying) {
            continue;
        }
        think(enemy, i, ticks, players, obstacles);
        if (enemy.expired) {
            die(enemy);
            continue;
        }
        // Nothing has been asked of a body just born: it walks off from its entrance.
        if (enemy.animator.entering()) {
            enemy.animator.request(EnemyAction::Walk);
        }
        enemy.animator.update(ticks, seconds, enemy.contact >= 0);
        enemy.threw = enemy.animator.threw();
        if (enemy.threw && missiles != nullptr) {
            shoot(enemy, i, players, *missiles, missileSpeedScale);
        }
        // Knock-back dies away, and what was thrown up comes down.
        enemy.push *= std::pow(kPushDecay, static_cast<f32>(ticks));
        if (std::abs(enemy.push.x) < kPushFloor) {
            enemy.push.x = 0.0f;
        }
        if (std::abs(enemy.push.z) < kPushFloor) {
            enemy.push.z = 0.0f;
        }
        enemy.push.y = std::max(enemy.push.y - kGravity * seconds, 0.0f);
    }
}

void Enemies::chooseTarget(Enemy& enemy, s32 slot, std::span<const EnemyView> players,
                           std::span<f32> crowding) {
    bool anyone = false;
    for (const EnemyView& view : players) {
        anyone = anyone || (!view.hidden && !view.invisible);
    }
    if (!anyone) {
        enemy.recognized = false;
    }
    // A mind looks round again every eighth frame, or at once when its player is gone.
    bool look =
        (m_frame % kRetargetEvery) == (static_cast<u32>(slot) % kRetargetEvery) || enemy.target < 0;
    if (enemy.target >= 0) {
        const EnemyView* current = viewOf(players, enemy.target);
        if (current == nullptr || current->hidden || current->invisible) {
            look = true;
        }
    }
    if (look) {
        enemy.targetBefore = enemy.target;
        enemy.target = -1;
        enemy.weightedDistance = 100000.0f;
        enemy.targetDistance = 100000.0f;
        for (const EnemyView& view : players) {
            if (view.hidden || view.invisible) {
                continue;
            }
            const f32 distance = flatDistance(view.position, enemy.position);
            if (distance > enemy.sight) {
                continue;
            }
            f32 weighted = distance;
            if (distance > 5.0f * enemy.radius && view.player >= 0 &&
                static_cast<usize>(view.player) < crowding.size()) {
                weighted += crowding[static_cast<usize>(view.player)];
            }
            if (weighted < enemy.weightedDistance) {
                enemy.weightedDistance = weighted;
                enemy.targetDistance = distance;
                enemy.target = view.player;
            }
        }
    } else if (const EnemyView* current = viewOf(players, enemy.target); current != nullptr) {
        enemy.targetDistance = flatDistance(current->position, enemy.position);
    }
    if (enemy.target >= 0) {
        if (enemy.targetDistance <= enemy.sight) {
            enemy.recognized = true;
            if (static_cast<usize>(enemy.target) < crowding.size()) {
                crowding[static_cast<usize>(enemy.target)] += 2.0f; // the next prefers another
            }
        }
    } else {
        enemy.targetDistance = 100000.0f;
        enemy.weightedDistance = 100000.0f;
    }
}

f32 Enemies::fightOf(const Enemy& enemy) const {
    const EnemyKind& kind = enemyKind(enemy.kind);
    const f32 fight = kind.damage * m_scales.damage;
    // A weakened one hits softer: two thirds under two thirds health, a third under a third.
    const f32 full = enemy.fullHealth;
    if (enemy.health > 0.667f * full || enemy.kind == kDeathKind) {
        return fight;
    }
    return enemy.health > 0.333f * full ? 0.667f * fight : 0.333f * fight;
}

void Enemies::resolveBlows(Enemy& enemy, s32 slot, std::span<const EnemyView> players) {
    (void)slot;
    const bool landed = enemy.animator.struck() || enemy.animator.powerStruck();
    if (!landed || enemy.attackIndex < 0) {
        if (landed) {
            enemy.attackIndex = -1;
        }
        return;
    }
    const EnemyView* victim = viewOf(players, enemy.attackIndex);
    if (victim != nullptr && !victim->hidden) {
        EnemyBlow blow;
        blow.player = enemy.attackIndex;
        blow.kind = enemy.kind;
        blow.tier = enemy.tier;
        blow.damage = fightOf(enemy);
        blow.power = enemy.animator.powerStruck();
        if (blow.power) {
            blow.damage *= kBlowGrowth;
            blow.knocksBack = enemy.reach > kKnockBackHeight;
        }
        const Vec3 toward = victim->position - enemy.position;
        const f32 length = flatDistance(victim->position, enemy.position);
        blow.direction = length > 0.001f ? Vec3{toward.x / length, 0.0f, toward.z / length}
                                         : Vec3{std::sin(enemy.yaw), 0.0f, std::cos(enemy.yaw)};
        m_blows.push_back(blow);
        ++enemy.attackCount;
    }
    enemy.attackIndex = -1;
}

void Enemies::react(Enemy& enemy) { // NOLINT(readability-convert-member-functions-to-static)
    if (enemy.hurtPending < 1.0f && enemy.health > 0.0f) {
        enemy.pushMagnitude = enemy.push.x * enemy.push.x + enemy.push.z * enemy.push.z;
        return;
    }
    const bool floors = (enemy.hurtFlags & EnemyHit::kFloors) != 0 ||
                        (enemy.hurtPending > 10.0f && (enemy.hurtFlags & EnemyHit::kMagic) != 0);
    f32 scale = 0.0f;
    if (floors) {
        enemy.animator.request(EnemyAction::HitReact2);
        scale = enemy.reach <= kKnockBackHeight ? 40.0f : 20.0f;
    } else if ((enemy.hurtFlags & EnemyHit::kKnockBack) != 0) {
        enemy.animator.request(EnemyAction::HitReact1);
        scale = 8.0f;
    } else {
        enemy.animator.request(EnemyAction::HitReact1);
    }
    if (scale > 0.0f) {
        enemy.push += enemy.hurtDirection * scale;
        ++enemy.pushes;
    }
    const f32 magnitude = glm::length(enemy.push);
    if (magnitude > kMostPush) {
        enemy.push *= kMostPush / magnitude;
    }
    enemy.hurtPending = 0.0f;
    enemy.hurtFlags = 0;
    enemy.hurtDirection = Vec3{0.0f, 0.0f, 0.0f};
    enemy.pushMagnitude = enemy.push.x * enemy.push.x + enemy.push.z * enemy.push.z;
    if (enemy.health <= 0.0f && enemy.state != State::Dying) {
        enemy.state = State::Dying;
        enemy.animator.request(EnemyAction::Dying);
    }
}

// ---- minds -------------------------------------------------------------------------------

f32 Enemies::turnToward(const Enemy& enemy, f32 wanted, s32 ticks) {
    const EnemyAction action = enemy.animator.action();
    if (action >= EnemyAction::HitReact1 || action == EnemyAction::Start) {
        return enemy.yaw;
    }
    f32 rate = enemyKind(enemy.kind).turnRate;
    if (action == EnemyAction::Run) {
        rate *= 3.0f;
    }
    const f32 step = rate * static_cast<f32>(ticks);
    const f32 d = wrapAngle(wanted - enemy.yaw);
    if (std::abs(d) <= step) {
        return wrapAngle(wanted);
    }
    return wrapAngle(enemy.yaw + (d > 0.0f ? step : -step));
}

/** Whether a step to `at` leads somewhere clear: the original sweeps the step for a wall
 * crossed, another enemy or an item in the way; here the point stepped to must not be in a
 * wall (a body already against one may still slide along it), off the floor, in a box or
 * in another. */
bool Enemies::probeClear(const Enemy& enemy, const Vec3& at, std::span<const Obstacle> obstacles,
                         s32 self) const {
    if (m_collision != nullptr) {
        const Vec3 pushed = m_collision->resolveWalls(at, kFootClearance, at.y + kFootClearance,
                                                      at.y + enemy.height - kFootClearance);
        if (flatDistance(pushed, at) > 0.01f) {
            return false;
        }
        if (!m_collision->floorAt(at, kStepUp, kDrop).has_value()) {
            return false;
        }
    }
    for (const Obstacle& box : obstacles) {
        if (box.solid && box.pushOut(at, enemy.radius) != at) {
            return false;
        }
    }
    for (s32 i = 0; i < m_most; ++i) {
        const Enemy& other = m_enemies[static_cast<usize>(i)];
        if (i == self || other.state == State::Inactive) {
            continue;
        }
        if (flatDistance(other.position, at) < enemy.radius + other.radius &&
            std::abs(other.position.y - at.y) < other.height) {
            return false;
        }
    }
    return true;
}

/** What the mind is given to go on this tick. */
MindSense Enemies::sense(const Enemy& enemy, s32 slot, s32 ticks,
                         std::span<const EnemyView> players,
                         std::span<const Obstacle> obstacles) const {
    MindSense sense;
    sense.position = enemy.position;
    sense.yaw = enemy.yaw;
    sense.radius = enemy.radius;
    sense.sight = enemy.sight;
    sense.pace = paceOf(enemy.kind);
    sense.ticks = ticks;
    sense.target = enemy.target;
    sense.targetDistance = enemy.targetDistance;
    sense.recognized = enemy.recognized;
    if (const EnemyView* view = viewOf(players, enemy.target); view != nullptr) {
        sense.targetPosition = view->position;
    }
    sense.contact = enemy.contact;
    if (const EnemyView* view = viewOf(players, enemy.contact); view != nullptr) {
        sense.contactPosition = view->position;
    }
    sense.bumpedWall = enemy.bumpedWall;
    sense.bumpedOther = enemy.bumpedOther;
    sense.blocked = enemy.blocked;
    sense.otherSide = enemy.otherSide;
    sense.generatorGone = enemy.generator < 0;
    sense.threw = enemy.threw;
    sense.idleTicks = enemy.idleTicks;
    sense.action = enemy.animator.action();
    if (const EnemyView* view = viewOf(players, enemy.target); view != nullptr) {
        sense.targetVertical = view->position.y - enemy.position.y;
    }
    const f32 reach = paceOf(enemy.kind) * static_cast<f32>(ticks);
    sense.clear = [this, &enemy, obstacles, slot, reach](f32 heading) {
        Vec3 probe = enemy.position;
        probe.x += reach * std::sin(heading);
        probe.z += reach * std::cos(heading);
        return probeClear(enemy, probe, obstacles, slot);
    };
    sense.open = [this, &enemy, reach](f32 heading) {
        if (m_collision == nullptr) {
            return true;
        }
        Vec3 probe = enemy.position;
        probe.x += reach * std::sin(heading);
        probe.z += reach * std::cos(heading);
        const Vec3 pushed = m_collision->resolveWalls(probe, enemy.radius * kWallRadiusScale,
                                                      probe.y + kFootClearance,
                                                      probe.y + enemy.height - kFootClearance);
        return flatDistance(pushed, probe) <= 0.01f &&
               m_collision->floorAt(probe, kStepUp, kDrop).has_value();
    };
    return sense;
}

/** The mind decides and the body carries it out: a step along the heading at the pace, a
 * turn toward it, the action asked of the animator, and perhaps a change of mind. */
void Enemies::think(Enemy& enemy, s32 slot, s32 ticks, std::span<const EnemyView> players,
                    std::span<const Obstacle> obstacles) {
    if (enemy.stunTicks > 0) {
        enemy.stunTicks -= ticks;
    }
    const MindIntent intent = enemyMindOf(enemy.algorithm)
                                  .think(enemy.mind, sense(enemy, slot, ticks, players, obstacles));
    if (intent.become.has_value()) {
        enemy.algorithm = *intent.become;
    }
    if (intent.expire) {
        enemy.expired = true;
        return;
    }
    if (intent.explode) {
        // It goes up: a blast where it stood, and itself dead of it.
        EnemyBurst burst;
        burst.position = bodyCentre(enemy);
        burst.damage = kSuicideDamage * m_scales.damage;
        burst.enemy = slot;
        m_bursts.push_back(burst);
        EnemyHit own;
        own.damage = 999.0f;
        own.player = -1;
        hurt(slot, own);
        react(enemy); // dead of it at once
        return;
    }
    if (intent.throwing) {
        enemy.animator.request(EnemyAction::Throw);
    }
    if (intent.pace > 0.0f) {
        enemy.animator.request(intent.action == EnemyAction::Walk && intent.pace >= kRunFrom
                                   ? EnemyAction::Run
                                   : intent.action);
    } else {
        enemy.animator.request(intent.action);
    }
    if (intent.turn) {
        enemy.yaw = turnToward(enemy, intent.heading, ticks);
    }
    const Vec3 step = Vec3{std::sin(intent.heading), 0.0f, std::cos(intent.heading)} *
                      (paceOf(enemy.kind) * intent.pace * static_cast<f32>(ticks));
    move(enemy, slot, ticks, static_cast<f32>(ticks) / static_cast<f32>(kTicksPerSecond), step,
         players, obstacles);
}

// ---- bodies ------------------------------------------------------------------------------

void Enemies::move(Enemy& enemy, s32 slot, s32 ticks, f32 seconds, const Vec3& step,
                   std::span<const EnemyView> players, std::span<const Obstacle> obstacles) {
    Vec3 translation = step;
    // Nothing of its own while stunned, reacting or swinging (a running attack runs on); a
    // push moves it regardless.
    const EnemyAction doing = enemy.animator.action();
    const bool runningAttack = doing == EnemyAction::RunAttack || doing == EnemyAction::RunAttack2;
    if (enemy.stunTicks > 0 || enemy.animator.reacting() ||
        (enemy.animator.swinging() && !runningAttack) || enemy.animator.entering()) {
        translation = Vec3{0.0f, 0.0f, 0.0f};
    }
    translation += enemy.push * seconds;
    // A touch of the world is remembered while the mind's hold on the heading runs, so
    // the mind sees it once more as the hold ends; a dead stop is this step's alone.
    if (enemy.mind.deadEnd <= 0) {
        enemy.bumpedWall = false;
    }
    enemy.bumpedOther = false;
    enemy.blocked = false;
    enemy.contact = -1;
    const Vec3 from = enemy.position;
    Vec3 to = from + translation;
    // Against a player it stops dead and strikes.
    for (const EnemyView& view : players) {
        if (view.hidden) {
            continue;
        }
        if (flatDistance(view.position, to) < view.radius + enemy.radius + 0.5f &&
            std::abs(view.position.y - to.y) < std::max(view.height, enemy.height)) {
            enemy.contact = view.player;
            break;
        }
    }
    if (enemy.contact >= 0) {
        if (const EnemyView* view = viewOf(players, enemy.contact); view != nullptr) {
            enemy.mind.route = turnDirection(from, view->position);
        }
        if (enemy.state == State::Active && enemy.algorithm != 31) {
            enemy.attackIndex = enemy.contact;
            enemy.animator.request((enemy.attackCount & 7) == 7 ? EnemyAction::PowerAttack
                                                                : EnemyAction::Attack);
        }
        return;
    }
    if (m_collision != nullptr && glm::length(translation) > 0.0f) {
        const f32 wallRadius = enemy.radius * kWallRadiusScale;
        Vec3 target = m_collision->resolveWalls(to, wallRadius, to.y + kFootClearance,
                                                to.y + enemy.height - kFootClearance);
        const bool wall = flatDistance(target, to) > 0.001f;
        const auto floor = m_collision->floorAt(target, kStepUp, kDrop);
        if (!floor.has_value()) {
            enemy.bumpedWall = true;
            enemy.blocked = true;
            return;
        }
        target.y = floor->y;
        if (wall) {
            // A slide along the wall that still gets somewhere is no bump; a dead stop is.
            const f32 kept = flatDistance(target, from);
            if (kept < kStopped) {
                enemy.bumpedWall = true;
                enemy.blocked = true;
                return;
            }
        }
        to = target;
    }
    for (const Obstacle& box : obstacles) {
        if (box.solid) {
            const Vec3 pushed = box.pushOut(to, enemy.radius);
            if (pushed != to) {
                enemy.bumpedWall = true;
                to = pushed;
            }
        }
    }
    // Against another it stops, unless it is being thrown, when half the push carries over.
    for (s32 i = 0; i < m_most; ++i) {
        Enemy& other = m_enemies[static_cast<usize>(i)];
        if (i == slot || other.state == State::Inactive || other.state == State::Asleep) {
            continue;
        }
        if (flatDistance(other.position, to) < enemy.radius + other.radius &&
            std::abs(other.position.y - to.y) < std::max(other.height, enemy.height)) {
            enemy.bumpedOther = true;
            if (enemy.pushMagnitude > 1.0f && enemy.animator.reacting()) {
                other.push += 0.5f * enemy.push;
                other.position += 0.5f * translation;
            } else {
                enemy.blocked = true;
                enemy.otherSide = turnDirection(from, other.position);
                return;
            }
        }
    }
    enemy.position = to;
    (void)ticks;
}

// ---- being hit ---------------------------------------------------------------------------

void Enemies::hurt(s32 id, const EnemyHit& hit) {
    if (id < 0 || id >= m_most) {
        return;
    }
    Enemy& enemy = m_enemies[static_cast<usize>(id)];
    if ((enemy.state != State::Active && enemy.state != State::Asleep) || enemy.killed) {
        return;
    }
    const EnemyKind& kind = enemyKind(enemy.kind);
    f32 amount = hit.damage;
    // A character under the level the place is meant for hits a hundredth softer a level;
    // one over it a tenth harder. Armour comes off, but a character always gets a point in.
    if (hit.player >= 0 && m_scales.playerLevel > 0.0f) {
        const f32 gap = static_cast<f32>(hit.level) - m_scales.playerLevel;
        amount *= gap < 0.0f ? 1.0f + 0.01f * gap : 1.0f + 0.1f * gap;
    }
    const Damage modified = Damage::modify(amount, hit.flags, 0, kind.armor, false);
    amount = std::max(modified.amount, hit.player >= 0 ? 1.0f : 0.0f);
    if (amount <= 0.0f) {
        return;
    }
    enemy.state = State::Active;
    enemy.health -= amount;
    enemy.hurtPending += amount;
    if ((modified.flags & Damage::kElement) != 0) {
        enemy.hurtFlags &= ~Damage::kElement;
    }
    enemy.hurtFlags |= modified.flags;
    const f32 length = glm::length(hit.direction);
    if (length > 0.001f) {
        enemy.hurtDirection = hit.direction / length;
    }
    enemy.hurtBy = hit.player;
    const bool killed = enemy.health <= 0.0f;
    ++enemy.hitCount;
    const EnemyFeedback feedback{enemy.kind,
                                 enemy.tier,
                                 enemy.hitCount,
                                 killed,
                                 hit.close,
                                 hit.flags,
                                 hit.where.has_value() && enemy.reach >= 4.0f
                                     ? *hit.where
                                     : enemy.position + Vec3{0, kind.attentionHeight, 0},
                                 enemy.yaw,
                                 enemy.reach};
    m_feedback.push_back(feedback);
    enemy.flashSeconds = killed ? 0.0f : 2.0f / 30.0f;
    if (killed) {
        enemy.killed = true;
        enemy.deathSkin = feedback.deathSkin();
        enemy.deathSkinFrames = feedback.deathSkinFrames();
    }
    if (hit.player >= 0) {
        EnemyLoss loss;
        loss.enemy = id;
        loss.kind = enemy.kind;
        loss.tier = enemy.tier;
        loss.player = hit.player;
        loss.killed = killed;
        loss.experience = killed ? kind.experienceKill : kind.experienceHit;
        loss.position = bodyCentre(enemy);
        m_losses.push_back(loss);
    }
}

void Enemies::die(Enemy& enemy) {
    enemy = Enemy{};
}

std::vector<EnemyFeedback> Enemies::takeFeedback() {
    std::vector<EnemyFeedback> out;
    out.swap(m_feedback);
    return out;
}

/** A shot or a lob at the player it is after, from its eyes to their middle. */
void Enemies::shoot(Enemy& enemy, s32 slot, std::span<const EnemyView> players,
                    EnemyMissiles& missiles, f32 speedScale) {
    Stock* stock = stockOf(enemy.kind);
    if (stock == nullptr) {
        return;
    }
    const EnemyKind& kind = enemyKind(enemy.kind);
    const Vec3 from = enemy.position + Vec3{0.0f, kind.attentionHeight, 0.0f};
    Vec3 aim = from + Vec3{std::sin(enemy.yaw), 0.0f, std::cos(enemy.yaw)} * 20.0f;
    if (const EnemyView* view = viewOf(players, enemy.target); view != nullptr) {
        aim = view->position + Vec3{0.0f, 0.5f * view->height, 0.0f};
    }
    // The slot its way throws from, and what the kind keeps there; the medium kinds' arrow
    // when it keeps nothing.
    const s32 which = enemy.variant == kBomberStrength ? EnemyMissileKind::kBomb
                                                       : missileSlotOfWay(enemy.algorithm);
    const EnemyMissileKind what =
        enemyMissileOf(enemy.kind, which).value_or(EnemyMissileKind::arrow());
    const TreeModel* model = which == EnemyMissileKind::kBomb ? &stock->bomb : &stock->arrow;
    missiles.launch(what, from, aim, speedScale, model->bound() ? model : nullptr, slot);
}

const TreeModel* Enemies::bodyOf(const Enemy& enemy) {
    Stock* stock = stockOf(enemy.kind);
    if (stock == nullptr) {
        return nullptr;
    }
    if (enemy.variant != 0) {
        const auto v = static_cast<usize>(enemy.variant - kArcherStrength);
        if (v < stock->variantBodies.size() && stock->variantBodies[v].bound()) {
            return &stock->variantBodies[v];
        }
    }
    s32 tier = enemy.tier;
    while (tier > 1 && stock->trees[static_cast<usize>(tier - 1)] == nullptr) {
        --tier;
    }
    const TreeModel& body = stock->bodies[static_cast<usize>(tier - 1)];
    return body.bound() ? &body : nullptr;
}

std::vector<EnemyBurst> Enemies::takeBursts() {
    return std::exchange(m_bursts, {});
}

std::vector<EnemyBlow> Enemies::takeBlows() {
    return std::exchange(m_blows, {});
}

std::vector<EnemyLoss> Enemies::takeLosses() {
    return std::exchange(m_losses, {});
}

std::vector<MissileTarget> Enemies::targets() const {
    std::vector<MissileTarget> out;
    for (s32 i = 0; i < m_most; ++i) {
        const Enemy& enemy = m_enemies[static_cast<usize>(i)];
        if (!alive(i)) {
            continue;
        }
        out.push_back(MissileTarget{i, enemy.position, enemy.radius, enemy.height});
    }
    return out;
}

std::optional<s32> Enemies::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    std::optional<s32> best;
    f32 bestDistance = 0.0f;
    const Vec3 sweep = to - from;
    const f32 length = glm::length(sweep);
    for (s32 i = 0; i < m_most; ++i) {
        const Enemy& enemy = m_enemies[static_cast<usize>(i)];
        if (enemy.state != State::Active && enemy.state != State::Asleep) {
            continue;
        }
        const Vec3 centre = bodyCentre(enemy);
        const f32 t =
            length > 0.001f
                ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f)
                : 0.0f;
        const Vec3 nearest = from + sweep * t;
        if (flatDistance(nearest, centre) <= radius + enemy.radius &&
            std::abs(nearest.y - centre.y) <= radius + enemy.reach) {
            const f32 distance = glm::length(nearest - from);
            if (!best.has_value() || distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
    }
    return best;
}

std::vector<s32> Enemies::within(const Vec3& centre, f32 radius) const {
    std::vector<s32> out;
    for (s32 i = 0; i < m_most; ++i) {
        const Enemy& enemy = m_enemies[static_cast<usize>(i)];
        if (enemy.state != State::Active && enemy.state != State::Asleep) {
            continue;
        }
        if (glm::length(bodyCentre(enemy) - centre) <= radius + enemy.radius) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<s32> Enemies::reachedBy(const Vec3& centre, f32 radius, f32 arc,
                                    const Vec3& facing) const {
    std::vector<s32> out;
    for (s32 i = 0; i < m_most; ++i) {
        const Enemy& enemy = m_enemies[static_cast<usize>(i)];
        if (enemy.state != State::Active && enemy.state != State::Asleep) {
            continue;
        }
        const Vec3 toward = enemy.position - centre;
        const f32 distance = flatDistance(enemy.position, centre);
        if (distance > radius + enemy.radius || centre.y > enemy.position.y + enemy.height ||
            centre.y < enemy.position.y - enemy.height) {
            continue;
        }
        if (arc < kPi && distance > 0.001f) {
            const f32 angle = std::acos(
                std::clamp((toward.x * facing.x + toward.z * facing.z) / distance, -1.0f, 1.0f));
            if (angle > arc) {
                continue;
            }
        }
        out.push_back(i);
    }
    return out;
}

// ---- looking -----------------------------------------------------------------------------

void Enemies::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                   const Texture* hitFlash, ItemArchive* weapons) {
    for (s32 i = 0; i < m_most; ++i) {
        const Enemy& enemy = m_enemies[static_cast<usize>(i)];
        if (enemy.state == State::Inactive || !enemy.animator.bound()) {
            continue;
        }
        const TreeModel* found = bodyOf(enemy);
        if (found == nullptr) {
            continue;
        }
        TreeModel& body =
            *const_cast<TreeModel*>(found); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        body.resetTextures();
        body.setAppearance(enemy.killed || enemy.flashSeconds > 0);
        if (enemy.flashSeconds > 0) {
            body.setMaskedTexture(hitFlash);
        }
        if (enemy.killed && !enemy.deathSkin.empty()) {
            ItemArchive* skins = enemy.deathSkin == "DEATHALT" ? archive(enemy.kind) : weapons;
            if (skins != nullptr) {
                for (const TextureAnimationInfo& animation : skins->trees.textureAnimations()) {
                    if (animation.name != enemy.deathSkin) {
                        continue;
                    }
                    const auto frame = static_cast<s32>(enemy.deathSeconds * kDeathSkinRate);
                    if (frame < enemy.deathSkinFrames && frame < animation.frames) {
                        const auto first =
                            animation.source >= 0
                                ? std::optional<u32>{static_cast<u32>(animation.source)}
                                : skins->textures.find(animation.frameName);
                        if (first.has_value() &&
                            *first + static_cast<u32>(frame) < skins->textures.size()) {
                            body.setMaskedTexture(
                                &skins->textures.texture(device, *first + static_cast<u32>(frame)));
                        }
                    }
                    break;
                }
            }
        }
        // The flip-book kinds change their whole mesh with the frame; the rest are posed.
        const AnimationPlayer& player = enemy.animator.player();
        body.setFrame(player.sequence(), static_cast<s32>(std::lround(player.frame())));
        const Mat4 model = glm::rotate(glm::translate(Mat4{1.0f}, enemy.position), enemy.yaw,
                                       Vec3{0.0f, 1.0f, 0.0f});
        body.draw(device, clip, model, lighting, enemy.animator.pose().matrices());
    }
}

bool Enemies::alive(s32 id) const {
    return id >= 0 && id < m_most && !m_enemies[static_cast<usize>(id)].killed &&
           (m_enemies[static_cast<usize>(id)].state == State::Active ||
            m_enemies[static_cast<usize>(id)].state == State::Asleep);
}

bool Enemies::dying(s32 id) const {
    return id >= 0 && id < m_most && m_enemies[static_cast<usize>(id)].state == State::Dying;
}

usize Enemies::count() const {
    usize n = 0;
    for (s32 i = 0; i < m_most; ++i) {
        n += m_enemies[static_cast<usize>(i)].state != State::Inactive ? 1 : 0;
    }
    return n;
}

s32 Enemies::kindOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].kind;
}
s32 Enemies::tierOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].tier;
}
s32 Enemies::generatorOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].generator;
}
f32 Enemies::healthOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].health;
}
const Vec3& Enemies::positionOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].position;
}
f32 Enemies::yawOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].yaw;
}
f32 Enemies::radiusOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].radius;
}
f32 Enemies::heightOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].height;
}
s32 Enemies::targetOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].target;
}
s32 Enemies::algorithmOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].algorithm;
}
s32 Enemies::pushCountOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].pushes;
}
s32 Enemies::variantOf(s32 id) const {
    return m_enemies[static_cast<usize>(id)].variant;
}

const EnemyAnimator* Enemies::animatorOf(s32 id) const {
    if (id < 0 || id >= m_most || m_enemies[static_cast<usize>(id)].state == State::Inactive) {
        return nullptr;
    }
    return &m_enemies[static_cast<usize>(id)].animator;
}

} // namespace gdl::game
