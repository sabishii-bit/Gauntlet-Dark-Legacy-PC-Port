#include "game/enemies/Critters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "game/enemies/EnemyMind.h"

namespace gdl::game {

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr f32 kStepUp = 2.0f;
constexpr f32 kDrop = 6.0f;
constexpr f32 kFootClearance = 0.1f;
constexpr f32 kPushDecay = 0.8f;
constexpr f32 kMostPush = 40.0f;
constexpr f32 kGravity = 100.0f;
constexpr f32 kKnockScale = 20.0f;
constexpr f32 kGolemKnockLoss = 5.0f; ///< a golem is that much harder to throw
constexpr f32 kDeathFade = 1.0f;      ///< seconds the fallen fades over after its death
constexpr f32 kBlindTurnShare = 0.1f; ///< of its turn rate while blinded

f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

f32 yawBetween(const Vec3& from, const Vec3& to) {
    return std::atan2(to.x - from.x, to.z - from.z);
}

} // namespace

std::string_view bossNameOf(s32 kind) {
    switch (kind) {
    case 34: return "DRAGON";
    case 35: return "CHIMERA";
    case 36: return "DJINN";
    case 37: return "DRIDER";
    case 38: return "PBOSS";
    case 39: return "YETI";
    case 40: return "WRAITH";
    case 41: return "LICH";
    case 42: return "SKORNE1";
    case 43: return "SKORNE2";
    case 44: return "GARM";
    default: return "";
    }
}

Critters::~Critters() {
    close();
}

void Critters::open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                    const WorldCollision* collision, const EnemyScales& scales, char realm) {
    close();
    m_device = &device;
    m_root = unpackedRoot;
    m_collision = collision;
    m_scales = scales;
    m_realm = static_cast<char>(std::toupper(static_cast<unsigned char>(realm)));
}

void Critters::close() {
    for (Critter& critter : m_critters) {
        critter = Critter{};
    }
    for (auto& stock : m_stocks) {
        stock->body.clear();
        stock->archive.release();
    }
    m_stocks.clear();
    m_blows.clear();
    m_losses.clear();
    m_device = nullptr;
    m_collision = nullptr;
}

const EnemyView* Critters::viewOf(std::span<const EnemyView> players, s32 player) {
    for (const EnemyView& view : players) {
        if (view.player == player) {
            return &view;
        }
    }
    return nullptr;
}

/** The data and archive of a kind, loaded on first asking: the golem's and general's costume
 * is the realm's (`MONSTERS/GOLEM/LEVELG`), a gargoyle's its form's (`MONSTERS/GAR_EAGL`). */
Critters::Stock* Critters::stockFor(s32 kind, std::string_view form) {
    std::string name;
    switch (kind) {
    case kGolemCritter: name = "GOLEM"; break;
    case kGeneralCritter: name = "GENERAL"; break;
    case kGargoyleCritter: name = form.empty() ? "GAR_EAGL" : normalizeAssetName(form); break;
    case kBossCritter:
        if (form.empty()) {
            return nullptr;
        }
        name = normalizeAssetName(form);
        break;
    default: return nullptr;
    }
    for (auto& stock : m_stocks) {
        if (stock->name == name) {
            return stock.get();
        }
    }
    if (m_device == nullptr) {
        return nullptr;
    }
    auto stock = std::make_unique<Stock>();
    stock->name = name;
    if (!stock->data.load(m_root / "critter" / (name + ".json"))) {
        return nullptr;
    }
    const std::filesystem::path archive =
        kind == kGargoyleCritter || kind == kBossCritter ? m_root / "MONSTERS" / name
                                 : m_root / "MONSTERS" / normalizeAssetName(stock->data.folder()) /
                                       std::format("LEVEL{}", m_realm);
    if (!stock->archive.load(archive)) {
        return nullptr;
    }
    const auto tree = stock->archive.trees.find(stock->data.tree());
    if (!tree.has_value()) {
        log::warn("critter {}: no tree {}", name, stock->data.tree());
        return nullptr;
    }
    stock->tree = &stock->archive.trees.tree(*tree);
    if (!stock->body.bind(*stock->tree, stock->archive.models, stock->archive.textures, *m_device)) {
        return nullptr;
    }
    m_stocks.push_back(std::move(stock));
    return m_stocks.back().get();
}

std::optional<s32> Critters::spawn(s32 kind, const Vec3& position, f32 yaw, std::string_view form) {
    Stock* stock = stockFor(kind, form);
    if (stock == nullptr) {
        return std::nullopt;
    }
    for (s32 i = 0; i < kMost; ++i) {
        Critter& critter = m_critters[static_cast<usize>(i)];
        if (critter.state != State::Inactive) {
            continue;
        }
        critter = Critter{};
        critter.state = State::Active;
        critter.stock = stock;
        critter.maxHealth = stock->data.maxHealth() * m_scales.health;
        critter.health = critter.maxHealth;
        critter.position = position;
        if (m_collision != nullptr) {
            if (const auto floor = m_collision->floorAt(position, kDrop, kDrop)) {
                critter.position.y = floor->y;
            }
        }
        critter.yaw = yaw;
        critter.cooldowns.assign(stock->data.moves().size(), 0.0f);
        // It comes in by its entrance, or its stance when it has none.
        const auto start = stock->data.moveOfType(CritterMove::kStart);
        const auto ready = stock->data.moveOfType(CritterMove::kReady);
        if (!(start.has_value() && startMove(critter, *start)) &&
            !(ready.has_value() && startMove(critter, *ready))) {
            critter = Critter{};
            return std::nullopt;
        }
        return i;
    }
    return std::nullopt;
}

bool Critters::startMove(Critter& critter, usize index) {
    const CritterData& data = critter.stock->data;
    if (index >= data.moves().size()) {
        return false;
    }
    const CritterMove& move = data.moves()[index];
    const auto sequence = critter.stock->tree->findSequence(move.anim);
    if (!sequence.has_value()) {
        return false;
    }
    critter.move = static_cast<s32>(index);
    critter.moveDone = false;
    critter.struckThisMove.clear();
    critter.player.start(critter.stock->tree->sequences[*sequence], *sequence);
    critter.pose.evaluate(*critter.stock->tree, *sequence, 0.0f);
    return true;
}

void Critters::chooseTarget(Critter& critter, std::span<const EnemyView> players) {
    critter.target = -1;
    critter.targetDistance = 100000.0f;
    const CritterTarget& sight = critter.stock->data.sight();
    for (const EnemyView& view : players) {
        if (view.hidden) {
            continue;
        }
        const f32 distance = flatDistance(view.position, critter.position);
        if (!sight.allows(distance, 0.0f, view.position.y - critter.position.y)) {
            continue;
        }
        if (distance < critter.targetDistance) {
            critter.targetDistance = distance;
            critter.target = view.player;
        }
    }
}

/** The loudest move whose target rule and cooldown allow it now. */
std::optional<usize> Critters::bestMove(const Critter& critter, std::span<const EnemyView> players) {
    const CritterData& data = critter.stock->data;
    const EnemyView* view = viewOf(players, critter.target);
    f32 distance = 100000.0f;
    f32 bearing = kPi;
    f32 vertical = 0.0f;
    if (view != nullptr) {
        distance = critter.targetDistance;
        bearing = wrapAngle(yawBetween(critter.position, view->position) - critter.yaw);
        vertical = view->position.y - critter.position.y;
    }
    std::optional<usize> best;
    s32 bestPriority = -1;
    for (usize i = 0; i < data.moves().size(); ++i) {
        const CritterMove& move = data.moves()[i];
        // Attacks, the steps (walks, turns and back-steps, types 48 to 63), the stance and
        // the taunt.
        const bool step = move.type >= CritterMove::kStepFrom && move.type < CritterMove::kStepTo;
        const bool considered = move.attack() || step || move.type == CritterMove::kReady ||
                                move.type == CritterMove::kTaunt;
        if (!considered || critter.cooldowns[i] > 0.0f) {
            continue;
        }
        // Attacks and walks want a player; the stance and the taunt want none in particular.
        if ((move.attack() || step) && view == nullptr) {
            continue;
        }
        if (!move.target.allows(distance, bearing, vertical) || curbedMove(critter, move)) {
            continue;
        }
        if (move.priority > bestPriority) {
            bestPriority = move.priority;
            best = i;
        }
    }
    return best;
}

/** A curbed attack is one whose harm (either part) carries the flag and is no burst. */
bool Critters::curbedMove(const Critter& critter, const CritterMove& move) {
    if (critter.curbSeconds <= 0.0f || !move.attack()) {
        return false;
    }
    return std::ranges::any_of(std::array{move.damage0, move.damage1}, [&](s32 index) {
        const CritterDamage* damage = critter.stock->data.damage(index);
        return damage != nullptr && (damage->flags & CritterDamage::kCurbed) != 0 &&
               damage->type != CritterDamage::kBurst;
    });
}

void Critters::chooseMove(Critter& critter, std::span<const EnemyView> players) {
    const CritterData& data = critter.stock->data;
    const CritterMove* current = critter.move >= 0 ? &data.moves()[static_cast<usize>(critter.move)] : nullptr;
    // What is loudest cuts in: the death, a roar after enough taken, a hit's reaction.
    const auto cutIn = [&](std::optional<usize> index) {
        if (!index.has_value() || (current != nullptr && !critter.moveDone &&
                                   data.moves()[*index].priority <= current->priority)) {
            return false;
        }
        if (startMove(critter, *index)) {
            critter.cooldowns[*index] = data.moves()[*index].cooldown;
            return true;
        }
        return false;
    };
    if (critter.state == State::Dying) {
        if (current == nullptr || current->type != CritterMove::kDeath) {
            if (!cutIn(data.moveOfType(CritterMove::kDeath))) {
                critter.moveDone = true;
            }
        }
        return;
    }
    if (critter.hurtPending >= 1.0f) {
        const bool floors = (critter.hurtFlags & EnemyHit::kFloors) != 0;
        const auto reaction = data.moveOfType(floors ? CritterMove::kKnockDown : CritterMove::kKnockBack);
        if (cutIn(reaction.has_value() ? reaction : data.moveOfType(CritterMove::kKnockBack))) {
            f32 scale = floors ? kKnockScale : 0.0f;
            if (data.kind() == kGolemCritter) {
                scale = std::max(scale - kGolemKnockLoss, 0.0f);
            }
            critter.push += critter.hurtDirection * scale;
            if (const f32 magnitude = glm::length(critter.push); magnitude > kMostPush) {
                critter.push *= kMostPush / magnitude;
            }
        }
        critter.hurtPending = 0.0f;
        critter.hurtFlags = 0;
    }
    if (critter.roarOwed >= kRoarAfter && cutIn(data.moveOfType(CritterMove::kRoar))) {
        critter.roarOwed = 0.0f;
    }
    // A move plays out, then what it links to, then whatever is best.
    if (current != nullptr && !critter.moveDone) {
        return;
    }
    if (current != nullptr && current->link >= 0 && startMove(critter, static_cast<usize>(current->link))) {
        return;
    }
    // Asked to roar, it does so before anything else; held, it keeps to its stance.
    if (critter.roarWanted) {
        critter.roarWanted = false;
        if (const auto bellow = data.moveOfType(CritterMove::kRoar);
            bellow.has_value() && startMove(critter, *bellow)) {
            return;
        }
    }
    if (critter.held) {
        if (const auto ready = data.moveOfType(CritterMove::kReady); ready.has_value()) {
            startMove(critter, *ready);
        }
        return;
    }
    if (const auto next = bestMove(critter, players); next.has_value()) {
        startMove(critter, *next);
        critter.cooldowns[*next] = data.moves()[*next].cooldown;
        return;
    }
    if (const auto ready = data.moveOfType(CritterMove::kReady); ready.has_value()) {
        startMove(critter, *ready);
    }
}

/** Where a named part of the body is now, in the world: the pose's node, or the feet. */
Vec3 Critters::partPosition(const Critter& critter, std::string_view node) {
    const Mat4 model = glm::rotate(glm::translate(Mat4{1.0f}, critter.position), critter.yaw,
                                   Vec3{0.0f, 1.0f, 0.0f});
    if (!node.empty()) {
        if (const auto index = critter.stock->tree->findNode(node); index.has_value()) {
            const std::span<const Mat4> matrices = critter.pose.matrices();
            if (*index < matrices.size()) {
                return Vec3{model * matrices[*index] * Vec4{0.0f, 0.0f, 0.0f, 1.0f}};
            }
        }
    }
    return Vec3{model * Vec4{critter.stock->data.originOffset(), 1.0f}};
}

/** The harm a move does over its active frames: a blow reaches whoever is within the
 * part's radius, a ring whoever is within its reach of the feet; each player once a move. */
void Critters::strikeWith(Critter& critter, s32 id, const CritterMove& move, s32 damageIndex,
                          std::span<const EnemyView> players) {
    const CritterDamage* damage = critter.stock->data.damage(damageIndex);
    if (damage == nullptr || damage->damage <= 0.0f) {
        return;
    }
    Vec3 centre;
    f32 reach = 0.0f;
    switch (damage->type) {
    case CritterDamage::kBlow:
        centre = partPosition(critter, move.colnode) + damage->offset;
        reach = damage->radius + damage->maxDistance;
        break;
    case CritterDamage::kRing:
    case CritterDamage::kBreath:
        centre = critter.position;
        reach = damage->maxDistance;
        break;
    default:
        return;
    }
    for (const EnemyView& view : players) {
        if (view.hidden || std::ranges::find(critter.struckThisMove, view.player) != critter.struckThisMove.end()) {
            continue;
        }
        const Vec3 feet = view.position;
        const Vec3 body = feet + Vec3{0.0f, 0.5f * view.height, 0.0f};
        const bool within = flatDistance(centre, feet) <= reach + view.radius &&
                            std::abs(centre.y - body.y) <= 0.5f * view.height + damage->radius;
        if (!within) {
            continue;
        }
        if (damage->type == CritterDamage::kBreath &&
            std::cos(wrapAngle(yawBetween(critter.position, feet) - critter.yaw)) < damage->minDot) {
            continue;
        }
        CritterBlow blow;
        blow.player = view.player;
        blow.critter = id;
        blow.damage = damage->damage * m_scales.damage;
        const Vec3 away = feet - critter.position;
        const f32 length = flatDistance(feet, critter.position);
        blow.direction = length > 0.001f ? Vec3{away.x / length, 0.0f, away.z / length}
                                         : Vec3{std::sin(critter.yaw), 0.0f, std::cos(critter.yaw)};
        m_blows.push_back(blow);
        critter.struckThisMove.push_back(view.player);
    }
}

/** The move carries the body: at its pace toward the player, turning at its rate. */
void Critters::carry(Critter& critter, f32 seconds, const CritterMove* move, std::span<const EnemyView> players) {
    const EnemyView* view = viewOf(players, critter.target);
    if (move != nullptr && move->turnRate > 0.0f && view != nullptr) {
        const f32 wanted = yawBetween(critter.position, view->position);
        const f32 d = wrapAngle(wanted - critter.yaw);
        // Blinded, it turns at a tenth of its rate.
        const f32 step = move->turnRate * seconds * (critter.blindTicks > 0 ? kBlindTurnShare : 1.0f);
        critter.yaw = wrapAngle(std::abs(d) <= step ? wanted : critter.yaw + (d > 0.0f ? step : -step));
    }
    Vec3 translation = critter.push * seconds;
    if (move != nullptr && move->speed > 0.0f && critter.state == State::Active) {
        const f32 pace = move->speed * m_scales.speed * seconds;
        translation += Vec3{std::sin(critter.yaw), 0.0f, std::cos(critter.yaw)} * pace;
    }
    if (glm::length(translation) <= 0.0f) {
        return;
    }
    Vec3 to = critter.position + translation;
    // Never onto a player: it stops against them.
    for (const EnemyView& other : players) {
        if (!other.hidden && flatDistance(other.position, to) < other.radius + critter.stock->data.radius()) {
            return;
        }
    }
    if (m_collision != nullptr) {
        const f32 wallRadius = critter.stock->data.wallRadius();
        to = m_collision->resolveWalls(to, wallRadius, to.y + kFootClearance, to.y + 8.0f);
        const auto floor = m_collision->floorAt(to, kStepUp, kDrop);
        if (!floor.has_value()) {
            return;
        }
        to.y = floor->y;
    }
    for (s32 i = 0; i < kMost; ++i) {
        const Critter& other = m_critters[static_cast<usize>(i)];
        if (&other == &critter || other.state == State::Inactive) {
            continue;
        }
        if (flatDistance(other.position, to) < other.stock->data.radius() + critter.stock->data.radius()) {
            return;
        }
    }
    critter.position = to;
}

void Critters::update(s32 ticks, f32 seconds, std::span<const EnemyView> players) {
    if (ticks <= 0) {
        return;
    }
    for (s32 i = 0; i < kMost; ++i) {
        Critter& critter = m_critters[static_cast<usize>(i)];
        if (critter.state == State::Inactive) {
            continue;
        }
        const CritterData& data = critter.stock->data;
        for (f32& cooldown : critter.cooldowns) {
            cooldown = std::max(cooldown - seconds, 0.0f);
        }
        // Frozen, it stands as it is: no move, no step, no one in its sights.
        if (critter.frozenTicks > 0) {
            critter.frozenTicks = std::max(critter.frozenTicks - ticks, 0);
            continue;
        }
        if (critter.state == State::Active) {
            chooseTarget(critter, players);
            if (critter.blindTicks > 0) {
                critter.blindTicks = std::max(critter.blindTicks - ticks, 0);
                critter.target = -1;
            }
        }
        chooseMove(critter, players);
        const CritterMove* move = critter.move >= 0 ? &data.moves()[static_cast<usize>(critter.move)] : nullptr;
        // The move plays; over its harmful frames its part strikes.
        if (move != nullptr && critter.player.playing()) {
            const f32 before = critter.player.frame();
            critter.player.advance(seconds, false);
            critter.moveDone = critter.player.finished();
            critter.pose.evaluate(*critter.stock->tree, critter.player.sequence(), critter.player.frame());
            const auto frame = static_cast<s32>(std::floor(critter.player.frame()));
            const auto active = [&](s32 start, s32 end) {
                const s32 last = end < start ? start : end;
                return start >= 0 && frame >= start && frame <= last;
            };
            if (critter.state == State::Active) {
                if (active(move->frameStart, move->frameEnd) && move->damage0 >= 0) {
                    strikeWith(critter, i, *move, move->damage0, players);
                }
                if (active(move->frameStart2, move->frameEnd2) && move->damage1 >= 0) {
                    strikeWith(critter, i, *move, move->damage1, players);
                }
            }
            (void)before;
        } else {
            critter.moveDone = true;
        }
        carry(critter, seconds, move, players);
        critter.push *= std::pow(kPushDecay, static_cast<f32>(ticks));
        critter.push.y = std::max(critter.push.y - kGravity * seconds, 0.0f);
        if (glm::length(critter.push) < 0.01f) {
            critter.push = Vec3{0.0f, 0.0f, 0.0f};
        }
        // The fallen fades once its death has played out, and is gone.
        if (critter.state == State::Dying && (move == nullptr || move->type != CritterMove::kDeath || critter.moveDone)) {
            critter.alpha -= seconds / kDeathFade;
            if (critter.alpha <= 0.0f) {
                critter = Critter{};
            }
        }
    }
}

void Critters::hurt(s32 id, const EnemyHit& hit) {
    if (id < 0 || id >= kMost) {
        return;
    }
    Critter& critter = m_critters[static_cast<usize>(id)];
    if (critter.state != State::Active) {
        return;
    }
    const CritterData& data = critter.stock->data;
    f32 amount = hit.damage;
    // A block lets a quarter through and shrugs off the throw.
    u32 flags = hit.flags;
    if (critter.move >= 0 && data.moves()[static_cast<usize>(critter.move)].type == CritterMove::kBlock) {
        amount *= kBlockShare;
        flags &= ~(EnemyHit::kFloors | EnemyHit::kKnockBack);
    }
    amount = std::max(amount - data.armor(), hit.player >= 0 ? 1.0f : 0.0f);
    if (amount <= 0.0f) {
        return;
    }
    critter.health -= amount;
    critter.hurtPending += amount;
    critter.hurtFlags |= flags;
    critter.roarOwed += amount;
    if (const f32 length = glm::length(hit.direction); length > 0.001f) {
        critter.hurtDirection = hit.direction / length;
    }
    // Every hit is worth its share of the creature's value to the one who dealt it, less a
    // fiftieth a level under the level the place is meant for.
    if (hit.player >= 0) {
        f32 share = amount / (1.0f + critter.maxHealth) * data.experience();
        if (m_scales.playerLevel > 0.0f && static_cast<f32>(hit.level) < m_scales.playerLevel) {
            const f32 under = m_scales.playerLevel - static_cast<f32>(hit.level);
            share *= std::max(1.0f - kUnderLevelLoss * under, 0.1f);
        }
        CritterLoss loss;
        loss.critter = id;
        loss.kind = data.kind();
        loss.player = hit.player;
        loss.experience = share;
        loss.position = critter.position;
        m_losses.push_back(loss);
    }
    if (critter.health <= 0.0f) {
        critter.state = State::Dying;
        CritterLoss fall;
        fall.critter = id;
        fall.kind = data.kind();
        fall.form = formOf(id);
        fall.player = -1;
        fall.experience = kKillShare * data.experience();
        fall.killed = true;
        fall.position = critter.position;
        m_losses.push_back(fall);
    }
}

std::vector<CritterBlow> Critters::takeBlows() {
    return std::exchange(m_blows, {});
}

std::vector<CritterLoss> Critters::takeLosses() {
    return std::exchange(m_losses, {});
}

std::vector<MissileTarget> Critters::targets() const {
    std::vector<MissileTarget> out;
    for (s32 i = 0; i < kMost; ++i) {
        const Critter& critter = m_critters[static_cast<usize>(i)];
        if (critter.state == State::Active) {
            out.push_back(MissileTarget{i, critter.position, critter.stock->data.radius(), 8.0f});
        }
    }
    return out;
}

std::optional<s32> Critters::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    std::optional<s32> best;
    f32 bestDistance = 0.0f;
    const Vec3 sweep = to - from;
    const f32 length = glm::length(sweep);
    for (s32 i = 0; i < kMost; ++i) {
        const Critter& critter = m_critters[static_cast<usize>(i)];
        if (critter.state != State::Active) {
            continue;
        }
        const Vec3 centre = critter.position + Vec3{0.0f, 4.0f, 0.0f};
        const f32 t = length > 0.001f ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f) : 0.0f;
        const Vec3 nearest = from + sweep * t;
        if (flatDistance(nearest, centre) <= radius + critter.stock->data.radius() &&
            std::abs(nearest.y - centre.y) <= radius + 4.0f) {
            const f32 distance = glm::length(nearest - from);
            if (!best.has_value() || distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
    }
    return best;
}

std::vector<s32> Critters::within(const Vec3& centre, f32 radius) const {
    std::vector<s32> out;
    for (s32 i = 0; i < kMost; ++i) {
        const Critter& critter = m_critters[static_cast<usize>(i)];
        if (critter.state == State::Active &&
            glm::length(critter.position + Vec3{0.0f, 4.0f, 0.0f} - centre) <= radius + critter.stock->data.radius()) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<s32> Critters::reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const {
    std::vector<s32> out;
    for (s32 i = 0; i < kMost; ++i) {
        const Critter& critter = m_critters[static_cast<usize>(i)];
        if (critter.state != State::Active) {
            continue;
        }
        const f32 distance = flatDistance(critter.position, centre);
        if (distance > radius + critter.stock->data.radius()) {
            continue;
        }
        if (arc < kPi && distance > 0.001f) {
            const Vec3 toward = critter.position - centre;
            const f32 angle = std::acos(std::clamp((toward.x * facing.x + toward.z * facing.z) / distance, -1.0f, 1.0f));
            if (angle > arc) {
                continue;
            }
        }
        out.push_back(i);
    }
    return out;
}

void Critters::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const Critter& critter : m_critters) {
        if (critter.state == State::Inactive || critter.stock == nullptr) {
            continue;
        }
        Mat4 model = glm::rotate(glm::translate(Mat4{1.0f}, critter.position), critter.yaw,
                                 Vec3{0.0f, 1.0f, 0.0f});
        if (critter.scale != 1.0f) {
            model = glm::scale(model, Vec3{critter.scale});
        }
        critter.stock->body.draw(device, clip, model, lighting, critter.pose.matrices(), nullptr,
                                 critter.alpha);
    }
}

Critters::Critter* Critters::critterAt(s32 id) {
    if (id < 0 || id >= kMost || m_critters[static_cast<usize>(id)].state == State::Inactive) {
        return nullptr;
    }
    return &m_critters[static_cast<usize>(id)];
}

void Critters::freeze(s32 id, s32 ticks) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->frozenTicks = std::max(ticks, 0);
    }
}

void Critters::blind(s32 id, s32 ticks) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->blindTicks = std::max(ticks, 0);
        critter->target = -1;
    }
}

void Critters::curb(s32 id, f32 seconds) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->curbSeconds = std::max(seconds, 0.0f);
    }
}

void Critters::resize(s32 id, f32 scale) {
    if (Critter* critter = critterAt(id); critter != nullptr && scale > 0.0f) {
        critter->scale = scale;
    }
}

void Critters::hold(s32 id, bool held) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->held = held;
    }
}

void Critters::roar(s32 id) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->roarWanted = true;
    }
}

s32 Critters::moveTypeOf(s32 id) const {
    const Critter& critter = m_critters[static_cast<usize>(id)];
    return critter.move >= 0 && critter.stock != nullptr
               ? critter.stock->data.moves()[static_cast<usize>(critter.move)].type
               : -1;
}

bool Critters::moveDoneOf(s32 id) const { return m_critters[static_cast<usize>(id)].moveDone; }
bool Critters::frozen(s32 id) const { return m_critters[static_cast<usize>(id)].frozenTicks > 0; }
bool Critters::blinded(s32 id) const { return m_critters[static_cast<usize>(id)].blindTicks > 0; }
bool Critters::curbed(s32 id) const { return m_critters[static_cast<usize>(id)].curbSeconds > 0.0f; }
f32 Critters::scaleOf(s32 id) const { return m_critters[static_cast<usize>(id)].scale; }

usize Critters::count() const {
    usize n = 0;
    for (const Critter& critter : m_critters) {
        n += critter.state != State::Inactive ? 1 : 0;
    }
    return n;
}

bool Critters::alive(s32 id) const {
    return id >= 0 && id < kMost && m_critters[static_cast<usize>(id)].state == State::Active;
}

bool Critters::dying(s32 id) const {
    return id >= 0 && id < kMost && m_critters[static_cast<usize>(id)].state == State::Dying;
}

s32 Critters::kindOf(s32 id) const { return m_critters[static_cast<usize>(id)].stock->data.kind(); }
f32 Critters::healthOf(s32 id) const { return m_critters[static_cast<usize>(id)].health; }
f32 Critters::maxHealthOf(s32 id) const { return m_critters[static_cast<usize>(id)].maxHealth; }
const Vec3& Critters::positionOf(s32 id) const { return m_critters[static_cast<usize>(id)].position; }
f32 Critters::yawOf(s32 id) const { return m_critters[static_cast<usize>(id)].yaw; }
f32 Critters::radiusOf(s32 id) const { return m_critters[static_cast<usize>(id)].stock->data.radius(); }
s32 Critters::targetOf(s32 id) const { return m_critters[static_cast<usize>(id)].target; }

std::string_view Critters::moveOf(s32 id) const {
    const Critter& critter = m_critters[static_cast<usize>(id)];
    if (critter.stock == nullptr || critter.move < 0) {
        return "";
    }
    return critter.stock->data.moves()[static_cast<usize>(critter.move)].name;
}

std::string Critters::formOf(s32 id) const {
    const Critter& critter = m_critters[static_cast<usize>(id)];
    if (critter.stock == nullptr || critter.stock->data.kind() != kGargoyleCritter) {
        return "";
    }
    const std::string& name = critter.stock->name; // "GAR_EAGL"
    const auto underscore = name.find('_');
    return underscore == std::string::npos ? name : name.substr(underscore + 1);
}

const CritterData* Critters::dataOf(s32 id) const {
    const Critter& critter = m_critters[static_cast<usize>(id)];
    return critter.stock != nullptr ? &critter.stock->data : nullptr;
}

} // namespace gdl::game
