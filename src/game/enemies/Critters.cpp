#include "game/enemies/Critters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Strings.h"

#include "game/enemies/CritterBreath.h"
#include "game/enemies/EnemyMind.h"

namespace gdl::game {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kStepUp = 2.0f;
constexpr float kDrop = 6.0f;
constexpr float kFootClearance = 0.1f;
constexpr float kPushDecay = 0.8f;
constexpr float kMostPush = 40.0f;
constexpr float kGravity = 100.0f;
constexpr float kKnockScale = 20.0f;
constexpr float kGolemKnockLoss = 5.0f; ///< a golem is that much harder to throw
constexpr float kDeathFade = 1.0f;      ///< seconds the fallen fades over after its death
constexpr float kBlindTurnShare = 0.1f; ///< of its turn rate while blinded
constexpr std::int32_t kThawBlinkTicks = 180;
constexpr std::int32_t kThawBlinkBit = 8;

float flatDistance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float yawBetween(const Vec3& from, const Vec3& to) {
    return std::atan2(to.x - from.x, to.z - from.z);
}

} // namespace

std::string_view bossNameOf(std::int32_t kind) {
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

const EnemyView* Critters::viewOf(std::span<const EnemyView> players, std::int32_t player) {
    for (const EnemyView& view : players) {
        if (view.player == player) {
            return &view;
        }
    }
    return nullptr;
}

/** The data and archive of a kind, loaded on first asking: the golem's and general's costume
 * is the realm's (`MONSTERS/GOLEM/LEVELG`), a gargoyle's its form's (`MONSTERS/GAR_EAGL`). */
Critters::Stock* Critters::stockFor(std::int32_t kind, std::string_view form) {
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
    const std::filesystem::path archive = kind == kGargoyleCritter || kind == kBossCritter
                                              ? m_root / "MONSTERS" / name
                                              : m_root / "MONSTERS" /
                                                    normalizeAssetName(stock->data.folder()) /
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
    if (!stock->body.bind(*stock->tree, stock->archive.models, stock->archive.textures,
                          *m_device)) {
        return nullptr;
    }
    m_stocks.push_back(std::move(stock));
    return m_stocks.back().get();
}

std::optional<std::int32_t> Critters::spawn(std::int32_t kind, const Vec3& position, float yaw,
                                            std::string_view form) {
    Stock* stock = stockFor(kind, form);
    if (stock == nullptr) {
        return std::nullopt;
    }
    for (std::int32_t i = 0; i < kMost; ++i) {
        Critter& critter = m_critters[static_cast<std::size_t>(i)];
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

bool Critters::startMove(Critter& critter, std::size_t index) {
    const CritterData& data = critter.stock->data;
    if (index >= data.moves().size()) {
        return false;
    }
    const CritterMove& move = data.moves()[index];
    const auto sequence = critter.stock->tree->findSequence(move.anim);
    if (!sequence.has_value()) {
        return false;
    }
    critter.move = static_cast<std::int32_t>(index);
    critter.moveDone = false;
    critter.struckThisMove.clear();
    critter.soundsGiven = 0;
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
        const float distance = flatDistance(view.position, critter.position);
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
std::optional<std::size_t> Critters::bestMove(const Critter& critter,
                                              std::span<const EnemyView> players) {
    const CritterData& data = critter.stock->data;
    const EnemyView* view = viewOf(players, critter.target);
    float distance = 100000.0f;
    float bearing = kPi;
    float vertical = 0.0f;
    if (view != nullptr) {
        distance = critter.targetDistance;
        bearing = wrapAngle(yawBetween(critter.position, view->position) - critter.yaw);
        vertical = view->position.y - critter.position.y;
    }
    std::optional<std::size_t> best;
    std::int32_t bestPriority = -1;
    for (std::size_t i = 0; i < data.moves().size(); ++i) {
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
    return std::ranges::any_of(std::array{move.damage0, move.damage1}, [&](std::int32_t index) {
        const CritterDamage* damage = critter.stock->data.damage(index);
        return damage != nullptr && (damage->flags & CritterDamage::kCurbed) != 0 &&
               damage->type != CritterDamage::kBurst;
    });
}

void Critters::chooseMove(Critter& critter, std::span<const EnemyView> players) {
    const CritterData& data = critter.stock->data;
    const CritterMove* current =
        critter.move >= 0 ? &data.moves()[static_cast<std::size_t>(critter.move)] : nullptr;
    // What is loudest cuts in: the death, a roar after enough taken, a hit's reaction.
    const auto cutIn = [&](std::optional<std::size_t> index) {
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
        const auto reaction =
            data.moveOfType(floors ? CritterMove::kKnockDown : CritterMove::kKnockBack);
        if (cutIn(reaction.has_value() ? reaction : data.moveOfType(CritterMove::kKnockBack))) {
            float scale = floors ? kKnockScale : 0.0f;
            if (data.kind() == kGolemCritter) {
                scale = std::max(scale - kGolemKnockLoss, 0.0f);
            }
            critter.push += critter.hurtDirection * scale;
            if (const float magnitude = glm::length(critter.push); magnitude > kMostPush) {
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
    if (current != nullptr && current->link >= 0 &&
        startMove(critter, static_cast<std::size_t>(current->link))) {
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

Mat4 Critters::modelTransform(const Critter& critter) {
    // The original places the root at floor Y + floorOffset, then transforms originOffset
    // and the animated nodes from that root. Keep our floor probes at the ground anchor:
    // the Dragon's root is 18.5 units above it, outside the probe's vertical search range.
    const Vec3 root = critter.position + Vec3{0.0f, critter.stock->data.floorOffset(), 0.0f};
    const Mat4 model =
        glm::rotate(glm::translate(Mat4{1.0f}, root), critter.yaw, Vec3{0.0f, 1.0f, 0.0f});
    return glm::scale(model, Vec3{critter.scale});
}

/** Where a named part is now: its animated node, or the type's body origin. */
Vec3 Critters::partPosition(const Critter& critter, std::string_view node) {
    return Vec3{partTransform(critter, node)[3]};
}

Mat4 Critters::partTransform(const Critter& critter, std::string_view node) {
    const Mat4 model = modelTransform(critter);
    if (!node.empty()) {
        if (const auto index = critter.stock->tree->findNode(node); index.has_value()) {
            const std::span<const Mat4> matrices = critter.pose.matrices();
            if (*index < matrices.size()) {
                return model * matrices[*index];
            }
        }
    }
    return glm::translate(model, critter.stock->data.originOffset());
}

std::optional<Mat4> Critters::nodeTransformOf(std::int32_t id, std::string_view node) const {
    if (id < 0 || id >= kMost) {
        return std::nullopt;
    }
    const Critter& critter = m_critters[static_cast<std::size_t>(id)];
    return critter.state != State::Inactive ? std::optional{partTransform(critter, node)}
                                            : std::nullopt;
}

/** Blows and rings hit each player once per move. Breath emits cylinder contacts
 * throughout its harmful frames; the recipient owns its repeated-damage timer. */
void Critters::strikeWith(Critter& critter, std::int32_t id, const CritterMove& move,
                          std::int32_t damageIndex, std::span<const EnemyView> players) {
    const CritterDamage* damage = critter.stock->data.damage(damageIndex);
    if (damage == nullptr || damage->damage <= 0.0f) {
        return;
    }
    Vec3 centre;
    float reach = 0.0f;
    std::optional<CritterBreath> breath;
    switch (damage->type) {
    case CritterDamage::kBlow:
        centre = partPosition(critter, move.colnode) + damage->offset;
        reach = damage->radius + damage->maxDistance;
        break;
    case CritterDamage::kRing:
        centre = critter.position;
        reach = damage->maxDistance;
        break;
    case CritterDamage::kBreath:
        breath = CritterBreath::fromNode(partTransform(critter, move.colnode), *damage);
        centre = breath->origin;
        break;
    default: return;
    }
    for (const EnemyView& view : players) {
        if (view.hidden ||
            (!breath.has_value() && std::ranges::find(critter.struckThisMove, view.player) !=
                                        critter.struckThisMove.end())) {
            continue;
        }
        const Vec3 feet = view.position;
        const Vec3 body = feet + Vec3{0.0f, 0.5f * view.height, 0.0f};
        const bool within =
            breath.has_value()
                ? breath->touches(*damage, body, view.radius, 0.5f * view.height)
                : flatDistance(centre, feet) <= reach + view.radius &&
                      std::abs(centre.y - body.y) <= 0.5f * view.height + damage->radius;
        if (!within) {
            continue;
        }
        CritterBlow blow;
        blow.player = view.player;
        blow.critter = id;
        blow.damage = damage->damage * m_scales.damage;
        blow.breath = breath.has_value();
        const Vec3 away = feet - critter.position;
        const float length = flatDistance(feet, critter.position);
        blow.direction = length > 0.001f ? Vec3{away.x / length, 0.0f, away.z / length}
                                         : Vec3{std::sin(critter.yaw), 0.0f, std::cos(critter.yaw)};
        if (breath.has_value()) {
            const Vec3 direction = breath->end - breath->origin;
            const float size = glm::length(direction);
            blow.direction = size > 0.0f ? direction / size : Vec3{0.0f};
        } else {
            critter.struckThisMove.push_back(view.player);
        }
        m_blows.push_back(blow);
    }
}

/** The move carries the body: at its pace toward the player, turning at its rate. */
void Critters::carry(Critter& critter, float seconds, const CritterMove* move,
                     std::span<const EnemyView> players) {
    const EnemyView* view = viewOf(players, critter.target);
    if (move != nullptr && move->turnRate > 0.0f && view != nullptr) {
        const float wanted = yawBetween(critter.position, view->position);
        const float d = wrapAngle(wanted - critter.yaw);
        // Blinded, it turns at a tenth of its rate.
        const float step =
            move->turnRate * seconds * (critter.blindTicks > 0 ? kBlindTurnShare : 1.0f);
        critter.yaw =
            wrapAngle(std::abs(d) <= step ? wanted : critter.yaw + (d > 0.0f ? step : -step));
    }
    Vec3 translation = critter.push * seconds;
    if (move != nullptr && move->speed > 0.0f && critter.state == State::Active) {
        const float pace = move->speed * m_scales.speed * seconds;
        translation += Vec3{std::sin(critter.yaw), 0.0f, std::cos(critter.yaw)} * pace;
    }
    if (glm::length(translation) <= 0.0f) {
        return;
    }
    Vec3 to = critter.position + translation;
    // Never onto a player: it stops against them.
    for (const EnemyView& other : players) {
        if (!other.hidden &&
            flatDistance(other.position, to) < other.radius + critter.stock->data.radius()) {
            return;
        }
    }
    if (m_collision != nullptr) {
        const float wallRadius = critter.stock->data.wallRadius();
        to = m_collision->resolveWalls(to, wallRadius, to.y + kFootClearance, to.y + 8.0f);
        const auto floor = m_collision->floorAt(to, kStepUp, kDrop);
        if (!floor.has_value()) {
            return;
        }
        to.y = floor->y;
    }
    for (std::int32_t i = 0; i < kMost; ++i) {
        const Critter& other = m_critters[static_cast<std::size_t>(i)];
        if (&other == &critter || other.state == State::Inactive) {
            continue;
        }
        if (flatDistance(other.position, to) <
            other.stock->data.radius() + critter.stock->data.radius()) {
            return;
        }
    }
    critter.position = to;
}

void Critters::update(std::int32_t ticks, float seconds, std::span<const EnemyView> players) {
    if (ticks <= 0) {
        return;
    }
    for (std::int32_t i = 0; i < kMost; ++i) {
        Critter& critter = m_critters[static_cast<std::size_t>(i)];
        if (critter.state == State::Inactive) {
            continue;
        }
        const CritterData& data = critter.stock->data;
        for (float& cooldown : critter.cooldowns) {
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
        const CritterMove* move =
            critter.move >= 0 ? &data.moves()[static_cast<std::size_t>(critter.move)] : nullptr;
        // The move plays; over its harmful frames its part strikes.
        if (move != nullptr && critter.player.playing()) {
            const float before = critter.player.frame();
            critter.player.advance(seconds, false);
            critter.moveDone = critter.player.finished();
            critter.pose.evaluate(*critter.stock->tree, critter.player.sequence(),
                                  critter.player.frame());
            const auto frame = static_cast<std::int32_t>(std::floor(critter.player.frame()));
            const auto active = [&](std::int32_t start, std::int32_t end) {
                const std::int32_t last = end < start ? start : end;
                return start >= 0 && frame >= start && frame <= last;
            };
            const auto isBreath = [&](std::int32_t index) {
                const CritterDamage* harm = data.damage(index);
                return harm != nullptr && harm->type == CritterDamage::kBreath;
            };
            const bool breathMove = isBreath(move->damage0) || isBreath(move->damage1);
            // Breath's effect starts at its authored sound frame and rides on the node.
            // Other attack effects retain their existing landing-frame presentation:
            // a swing's glow or a stomp's ring waits while its sound starts earlier.
            const auto giveOnce = [&](std::uint32_t bit, std::int32_t sound, const Vec3& where,
                                      CueParts parts) {
                if (sound >= 0 && (critter.soundsGiven & bit) == 0) {
                    critter.soundsGiven |= bit;
                    const auto node = breathMove && (bit == 1U || bit == 2U)
                                          ? std::optional<std::string_view>{move->colnode}
                                          : std::nullopt;
                    cue(critter, i, sound, where, parts, node);
                }
            };
            const bool lands = !breathMove && move->attack() && move->frameStart > move->soundFrame;
            if (frame >= move->soundFrame) {
                giveOnce(1U, move->sound, critter.position,
                         lands ? CueParts::Sound : CueParts::Both);
            }
            if (lands && frame >= move->frameStart) {
                giveOnce(16U, move->sound, critter.position, CueParts::Effect);
            }
            if (frame >= move->sound2Frame) {
                giveOnce(2U, move->sound2, critter.position, CueParts::Both);
            }
            if (critter.state == State::Active) {
                if (active(move->frameStart, move->frameEnd) && move->damage0 >= 0) {
                    if (const CritterDamage* harm = data.damage(move->damage0); harm != nullptr) {
                        giveOnce(4U, harm->sound,
                                 partPosition(critter, move->colnode) + harm->offset,
                                 CueParts::Both);
                    }
                    strikeWith(critter, i, *move, move->damage0, players);
                }
                if (active(move->frameStart2, move->frameEnd2) && move->damage1 >= 0) {
                    if (const CritterDamage* harm = data.damage(move->damage1); harm != nullptr) {
                        giveOnce(8U, harm->sound,
                                 partPosition(critter, move->colnode) + harm->offset,
                                 CueParts::Both);
                    }
                    strikeWith(critter, i, *move, move->damage1, players);
                }
            } else if (critter.state == State::Dying && active(move->frameStart, move->frameEnd) &&
                       (critter.soundsGiven & 32U) == 0) {
                // The death's harm is not a strike but a throw: what it spews goes out
                // once, the moment its frame comes.
                if (const CritterDamage* harm = data.damage(move->damage0);
                    harm != nullptr && harm->type == CritterDamage::kSpew) {
                    critter.soundsGiven |= 32U;
                    m_spews.push_back(CritterSpew{i, critter.position,
                                                  harm->spewVelocity(critter.yaw),
                                                  harm->spewHalfAngle()});
                }
            }
            (void)before;
        } else {
            critter.moveDone = true;
        }
        carry(critter, seconds, move, players);
        critter.push *= std::pow(kPushDecay, static_cast<float>(ticks));
        critter.push.y = std::max(critter.push.y - kGravity * seconds, 0.0f);
        if (glm::length(critter.push) < 0.01f) {
            critter.push = Vec3{0.0f, 0.0f, 0.0f};
        }
        // The fallen fades once its death has played out, and is gone.
        if (critter.state == State::Dying &&
            (move == nullptr || move->type != CritterMove::kDeath || critter.moveDone)) {
            critter.alpha -= seconds / kDeathFade;
            if (critter.alpha <= 0.0f) {
                critter = Critter{};
            }
        }
    }
}

void Critters::hurt(std::int32_t id, const EnemyHit& hit) {
    if (id < 0 || id >= kMost) {
        return;
    }
    Critter& critter = m_critters[static_cast<std::size_t>(id)];
    if (critter.state != State::Active) {
        return;
    }
    const CritterData& data = critter.stock->data;
    float amount = hit.damage;
    // A block lets a quarter through and shrugs off the throw.
    std::uint32_t flags = hit.flags;
    if (critter.move >= 0 &&
        data.moves()[static_cast<std::size_t>(critter.move)].type == CritterMove::kBlock) {
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
    if (const float length = glm::length(hit.direction); length > 0.001f) {
        critter.hurtDirection = hit.direction / length;
    }
    // Where it was struck, its own mark of a hit: a blow's or a missile's.
    const std::int32_t mark =
        hit.close && data.hitSoundClose() >= 0 ? data.hitSoundClose() : data.hitSoundFar();
    cue(critter, id, mark, hit.where.value_or(partPosition(critter, {})));
    // Every hit is worth its share of the creature's value to the one who dealt it, less a
    // fiftieth a level under the level the place is meant for.
    if (hit.player >= 0) {
        float share = amount / (1.0f + critter.maxHealth) * data.experience();
        if (m_scales.playerLevel > 0.0f && static_cast<float>(hit.level) < m_scales.playerLevel) {
            const float under = m_scales.playerLevel - static_cast<float>(hit.level);
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

/** A sound record and what it links to become cues: the tree at `position` (offset the
 * record's way, turned with the body), riding the body when the record says so, and the
 * sound named for the level. */
void Critters::cue(const Critter& critter, std::int32_t id, std::int32_t index,
                   const Vec3& position, CueParts parts, std::optional<std::string_view> node) {
    const CritterData& data = critter.stock->data;
    for (std::int32_t at = index, guard = 0; at >= 0 && guard < 8; ++guard) {
        const CritterSound* record = data.sound(at);
        if (record == nullptr) {
            break;
        }
        CritterCue out;
        out.critter = id;
        if (parts != CueParts::Sound) {
            out.tree = record->shows() ? record->tree : std::string{};
        }
        if (parts != CueParts::Effect) {
            out.sound = record->soundFor(m_realm);
        }
        const float sy = std::sin(critter.yaw);
        const float cy = std::cos(critter.yaw);
        const Vec3 turned{record->offset.x * cy + record->offset.z * sy, record->offset.y,
                          record->offset.z * cy - record->offset.x * sy};
        out.position = position + turned;
        out.yaw = critter.yaw;
        out.scale = record->scale * critter.scale;
        out.life = record->life;
        out.follows = record->follows();
        out.shakes = (record->flags & CritterSound::kShakes) != 0;
        // Without a root/entity/global parenting override, a move effect uses its
        // active animated node. Hit marks have no requested attachment.
        constexpr std::uint32_t kAlternateParent = 0x2000U | 0x800U | 0x40U | 1U;
        if (node.has_value() && !out.tree.empty() && (record->flags & kAlternateParent) == 0) {
            out.node = *node;
            out.nodeOffset = record->offset;
            out.position = Vec3{partTransform(critter, *node) * Vec4{record->offset, 1.0f}};
            out.scale = record->scale; // creature scale is already in the parent matrix
            out.follows = true;
        }
        if (!out.tree.empty() || !out.sound.empty()) {
            m_cues.push_back(std::move(out));
        }
        at = record->link;
    }
}

std::vector<CritterCue> Critters::takeCues() {
    return std::exchange(m_cues, {});
}

std::vector<CritterBlow> Critters::takeBlows() {
    return std::exchange(m_blows, {});
}

std::vector<CritterLoss> Critters::takeLosses() {
    return std::exchange(m_losses, {});
}

std::vector<CritterSpew> Critters::takeSpews() {
    return std::exchange(m_spews, {});
}

std::vector<MissileTarget> Critters::targets() const {
    std::vector<MissileTarget> out;
    for (std::int32_t i = 0; i < kMost; ++i) {
        const Critter& critter = m_critters[static_cast<std::size_t>(i)];
        if (critter.state == State::Active) {
            out.push_back(MissileTarget{i, critter.position, critter.stock->data.radius(), 8.0f});
        }
    }
    return out;
}

std::optional<std::int32_t> Critters::struckBy(const Vec3& from, const Vec3& to,
                                               float radius) const {
    std::optional<std::int32_t> best;
    float bestDistance = 0.0f;
    const Vec3 sweep = to - from;
    const float length = glm::length(sweep);
    for (std::int32_t i = 0; i < kMost; ++i) {
        const Critter& critter = m_critters[static_cast<std::size_t>(i)];
        if (critter.state != State::Active) {
            continue;
        }
        const Vec3 centre = critter.position + Vec3{0.0f, 4.0f, 0.0f};
        const float t =
            length > 0.001f
                ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f)
                : 0.0f;
        const Vec3 nearest = from + sweep * t;
        if (flatDistance(nearest, centre) <= radius + critter.stock->data.radius() &&
            std::abs(nearest.y - centre.y) <= radius + 4.0f) {
            const float distance = glm::length(nearest - from);
            if (!best.has_value() || distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
    }
    return best;
}

std::vector<std::int32_t> Critters::within(const Vec3& centre, float radius) const {
    std::vector<std::int32_t> out;
    for (std::int32_t i = 0; i < kMost; ++i) {
        const Critter& critter = m_critters[static_cast<std::size_t>(i)];
        if (critter.state == State::Active &&
            glm::length(critter.position + Vec3{0.0f, 4.0f, 0.0f} - centre) <=
                radius + critter.stock->data.radius()) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<std::int32_t> Critters::reachedBy(const Vec3& centre, float radius, float arc,
                                              const Vec3& facing) const {
    std::vector<std::int32_t> out;
    for (std::int32_t i = 0; i < kMost; ++i) {
        const Critter& critter = m_critters[static_cast<std::size_t>(i)];
        if (critter.state != State::Active) {
            continue;
        }
        const float distance = flatDistance(critter.position, centre);
        if (distance > radius + critter.stock->data.radius()) {
            continue;
        }
        if (arc < kPi && distance > 0.001f) {
            const Vec3 toward = critter.position - centre;
            const float angle = std::acos(
                std::clamp((toward.x * facing.x + toward.z * facing.z) / distance, -1.0f, 1.0f));
            if (angle > arc) {
                continue;
            }
        }
        out.push_back(i);
    }
    return out;
}

void Critters::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                    const Texture* frozenTexture) const {
    for (const Critter& critter : m_critters) {
        if (critter.state == State::Inactive || critter.stock == nullptr) {
            continue;
        }
        // The model is shared by this species, but object-frame selection belongs to the
        // individual. Set it for every draw, including the first and frozen frames.
        critter.stock->body.setFrame(critter.player.sequence(),
                                     static_cast<std::int32_t>(critter.player.frame()));
        critter.stock->body.resetTextures();
        // Retail flashes the normal skin on bit 3 in the final 180 frozen ticks.
        if (frozenTexture != nullptr && critter.frozenTicks > 0 &&
            (critter.frozenTicks >= kThawBlinkTicks ||
             (critter.frozenTicks & kThawBlinkBit) == 0)) {
            critter.stock->body.setMaskedTexture(frozenTexture);
        }
        critter.stock->body.draw(device, clip, modelTransform(critter), lighting,
                                 critter.pose.matrices(), nullptr, critter.alpha);
    }
}

Critters::Critter* Critters::critterAt(std::int32_t id) {
    if (id < 0 || id >= kMost ||
        m_critters[static_cast<std::size_t>(id)].state == State::Inactive) {
        return nullptr;
    }
    return &m_critters[static_cast<std::size_t>(id)];
}

void Critters::freeze(std::int32_t id, std::int32_t ticks) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->frozenTicks = std::max(ticks, 0);
        critter->roarWanted = false;
    }
}

void Critters::blind(std::int32_t id, std::int32_t ticks) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->blindTicks = std::max(ticks, 0);
        critter->target = -1;
    }
}

void Critters::curb(std::int32_t id, float seconds) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->curbSeconds = std::max(seconds, 0.0f);
    }
}

void Critters::resize(std::int32_t id, float scale) {
    if (Critter* critter = critterAt(id); critter != nullptr && scale > 0.0f) {
        critter->scale = scale;
    }
}

void Critters::hold(std::int32_t id, bool held) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->held = held;
    }
}

void Critters::roar(std::int32_t id) {
    if (Critter* critter = critterAt(id); critter != nullptr) {
        critter->roarWanted = true;
    }
}

std::int32_t Critters::moveTypeOf(std::int32_t id) const {
    const Critter& critter = m_critters[static_cast<std::size_t>(id)];
    return critter.move >= 0 && critter.stock != nullptr
               ? critter.stock->data.moves()[static_cast<std::size_t>(critter.move)].type
               : -1;
}

bool Critters::moveDoneOf(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].moveDone;
}
bool Critters::frozen(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].frozenTicks > 0;
}
bool Critters::blinded(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].blindTicks > 0;
}
bool Critters::curbed(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].curbSeconds > 0.0f;
}
float Critters::scaleOf(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].scale;
}

std::size_t Critters::count() const {
    std::size_t n = 0;
    for (const Critter& critter : m_critters) {
        n += critter.state != State::Inactive ? 1 : 0;
    }
    return n;
}

bool Critters::alive(std::int32_t id) const {
    return id >= 0 && id < kMost && m_critters[static_cast<std::size_t>(id)].state == State::Active;
}

bool Critters::dying(std::int32_t id) const {
    return id >= 0 && id < kMost && m_critters[static_cast<std::size_t>(id)].state == State::Dying;
}

std::int32_t Critters::kindOf(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].stock->data.kind();
}
float Critters::healthOf(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].health;
}
float Critters::maxHealthOf(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].maxHealth;
}
const Vec3& Critters::positionOf(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].position;
}
float Critters::yawOf(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].yaw;
}
float Critters::radiusOf(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].stock->data.radius();
}
std::int32_t Critters::targetOf(std::int32_t id) const {
    return m_critters[static_cast<std::size_t>(id)].target;
}

std::string_view Critters::moveOf(std::int32_t id) const {
    const Critter& critter = m_critters[static_cast<std::size_t>(id)];
    if (critter.stock == nullptr || critter.move < 0) {
        return "";
    }
    return critter.stock->data.moves()[static_cast<std::size_t>(critter.move)].name;
}

std::string Critters::formOf(std::int32_t id) const {
    const Critter& critter = m_critters[static_cast<std::size_t>(id)];
    if (critter.stock == nullptr || critter.stock->data.kind() != kGargoyleCritter) {
        return "";
    }
    const std::string& name = critter.stock->name; // "GAR_EAGL"
    const auto underscore = name.find('_');
    return underscore == std::string::npos ? name : name.substr(underscore + 1);
}

const CritterData* Critters::dataOf(std::int32_t id) const {
    const Critter& critter = m_critters[static_cast<std::size_t>(id)];
    return critter.stock != nullptr ? &critter.stock->data : nullptr;
}

ItemArchive* Critters::archiveOf(std::int32_t id) {
    Critter* critter = critterAt(id);
    return critter != nullptr ? &critter->stock->archive : nullptr;
}

} // namespace gdl::game
