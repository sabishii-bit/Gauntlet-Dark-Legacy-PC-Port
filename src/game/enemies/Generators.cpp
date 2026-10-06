#include "game/enemies/Generators.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>

#include "engine/core/Types.h"

#include "game/world/ItemSupport.h"
#include "game/world/TargetAssist.h"

namespace gdl::game {

namespace {
constexpr usize kGeneratorKinds = 34;
/** lbl_8011BB20: a hit, by the kind bred. */
constexpr std::array<s32, kGeneratorKinds> kHitWorth{1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3,
                                                     1, 2, 2, 1, 2, 3, 1, 2, 3, 1, 1, 2,
                                                     3, 3, 3, 1, 0, 2, 1, 1, 2, 2};
/** lbl_8011BBA8: the destroying blow. */
constexpr std::array<s32, kGeneratorKinds> kDestroyedWorth{1, 2, 3, 1,  2, 3,  1, 2, 3,   1, 2, 3,
                                                           1, 2, 2, 1,  2, 3,  1, 2, 3,   1, 1, 2,
                                                           3, 3, 3, 15, 0, 30, 1, 2, 300, 30};
constexpr s32 kExperienceShare = 5;
} // namespace

s32 generatorExperience(s32 kind, bool destroyed) {
    constexpr s32 kSecondUnknown = -2;
    constexpr s32 kThirdUnknown = -3;
    if (kind == kSecondUnknown) {
        kind = 1;
    } else if (kind == kThirdUnknown) {
        kind = 2;
    } else if (kind < 0) {
        kind = 0;
    }
    if (static_cast<usize>(kind) >= kGeneratorKinds) {
        return 0;
    }
    const auto& worth = destroyed ? kDestroyedWorth : kHitWorth;
    return kExperienceShare * worth[static_cast<usize>(kind)];
}

namespace {

constexpr std::array<s32, 4> kTempleKinds{16, 23, 14, 13};
constexpr std::array<s32, 4> kHellKinds{2, 24, 20, 25};
constexpr std::array<s32, 4> kHellAlgorithms{30, 30, 7, 7};
// A generator's kind resolves as though bred at no strength: never the medium's second row.
constexpr s32 kGeneratorKindStrength = 0;
constexpr std::array<s32, 3> kCasterAlgorithms{28, 29, 30}; ///< dropped once a state crumbles
constexpr f32 kGeneratorViewScale = 8.0f;

f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

s32 Generators::paramOf(const ItemInstance& instance, usize index) {
    if (index * 2 + 1 >= instance.params.size()) {
        return 0;
    }
    s16 value = 0;
    std::memcpy(&value, &instance.params[index * 2], sizeof(value));
    return value;
}

Generators::Bodies* Generators::bodiesOf(s32 kind) {
    for (auto& bodies : m_bodies) {
        if (bodies->kind == kind) {
            return bodies.get();
        }
    }
    return nullptr;
}

const Generators::Bodies* Generators::bodiesOf(s32 kind) const {
    for (const auto& bodies : m_bodies) {
        if (bodies->kind == kind) {
            return bodies.get();
        }
    }
    return nullptr;
}

bool Generators::loadBodies(RenderDevice& device, Enemies& enemies, s32 kind,
                            ItemArchive* realmItems) {
    if (bodiesOf(kind) != nullptr) {
        return true;
    }
    if (kind >= 0 && !enemies.loadKind(kind)) {
        return false;
    }
    ItemArchive* archive = kind < -1 ? realmItems : enemies.archive(kind);
    if (archive == nullptr) {
        return false;
    }
    auto bodies = std::make_unique<Bodies>();
    bodies->kind = kind;
    const EnemyKind& info = enemyKind(kind);
    // The state's object: "GEN_GRU3", tried with the level-one and root suffixes as the
    // original does. Whole is three; nought is the ruin.
    for (s32 state = 0; state <= kStates; ++state) {
        const std::string base =
            std::format("GEN_{}{}", kind < -1 ? "SPECIAL" : info.prefix, state);
        // SPECIAL generators are multi-node authored trees in the realm archive,
        // not a model from one of the four enemy species they produce.
        if (const auto tree = archive->trees.find(base)) {
            bodies->models[static_cast<usize>(state)].bind(
                archive->trees.tree(*tree), archive->models, archive->textures, device);
            bodies->models[static_cast<usize>(state)].setFrame(0, 0);
            continue;
        }
        for (const char* suffix : {"L1", "", "ROOT"}) {
            const auto object = archive->models.find(base + suffix);
            if (!object.has_value()) {
                continue;
            }
            const u32 objectIndex = *object;
            TreeInfo& tree = bodies->trees[static_cast<usize>(state)];
            tree.name = base;
            TreeNodeInfo node;
            node.name = base;
            node.object = archive->models.entry(objectIndex).name;
            tree.nodes.push_back(node);
            bodies->models[static_cast<usize>(state)].bind(tree, archive->models, archive->textures,
                                                           device);
            break;
        }
    }
    m_bodies.push_back(std::move(bodies));
    return true;
}

bool Generators::bind(RenderDevice& device, const WorldLayout& layout, Enemies& enemies,
                      const WorldCollision* collision, const GeneratorScales& scales, s32 players,
                      std::span<const LevelEnemy> roster, s32 realm, ItemArchive* realmItems) {
    clear();
    m_collision = collision;
    const auto authored = itemSupportWorld(layout, collision);
    m_scales = scales;
    m_players = players;
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    for (const ItemInstance& instance : layout.itemInstances()) {
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kGenerator) {
            continue;
        }
        const bool special = info.name == "SPECIAL" && (realm == 5 || realm == 6);
        const auto named =
            special ? std::optional<s32>{realm == 5 ? -2 : -3} : enemyKindOf(info.name);
        if (!named.has_value()) {
            continue;
        }
        // Small-class placements use the LOW body before the level remaps their species.
        const ItemInfo* body = &info;
        if (*named == kRatKind) {
            const auto low =
                std::ranges::find_if(layout.itemInfos(), [&](const ItemInfo& candidate) {
                    return candidate.type == info.type && candidate.name == "LOW" &&
                           (info.subtype <= 0 || candidate.subtype == info.subtype);
                });
            if (low != layout.itemInfos().end()) {
                body = &*low;
            }
        }
        const s32 specialTier = realm == 5 ? 2 : 3;
        const s32 authoredStrength = std::max(paramOf(instance, 0), 1);
        const s32 strength = special ? specialTier : authoredStrength;
        const s32 kind = levelKindOf(roster, *named, kGeneratorKindStrength);
        if (special) {
            for (const s32 species : realm == 5 ? kTempleKinds : kHellKinds) {
                enemies.loadKind(species);
            }
        }
        if (!loadBodies(device, enemies, kind, realmItems)) {
            continue;
        }
        Generator generator;
        generator.minPlayers = instance.minPlayers;
        generator.presence = shownToParty(instance.minPlayers, players)
                                 ? Generator::Presence::Shown
                                 : Generator::Presence::Hidden;
        generator.kind = kind;
        // The strength is also the state it stands in: a strength-one generator is one state
        // from gone and looks it.
        generator.tier = strength;
        generator.state = std::min(strength, kStates);
        generator.algorithm = paramOf(instance, 1);
        if (generator.algorithm < 0) {
            generator.algorithm = enemyKind(kind).algorithm;
        }
        const auto tierIndex = static_cast<usize>(std::clamp(authoredStrength, 1, kStates) - 1);
        s32 most = paramOf(instance, 2);
        s32 interval = paramOf(instance, 3);
        if (most == 0) {
            most = kDefaultMost[tierIndex];
        }
        if (interval == 0) {
            interval = kDefaultInterval[tierIndex];
        }
        // Scaled the way the original truncates them: to a whole count and interval.
        generator.most = static_cast<s32>(static_cast<f32>(most) * scales.most);
        generator.interval = static_cast<s32>(static_cast<f32>(interval) * scales.rate);
        generator.threshold = std::trunc(static_cast<f32>(body->hitPoints) * scales.health);
        // Durability and defaults use the authored strength before a special brood
        // replaces its troop tier. Damage and initial health are whole hit points.
        generator.health =
            std::trunc(static_cast<f32>(info.hitPoints * authoredStrength) * scales.health);
        generator.armor = static_cast<f32>(info.armor);
        const Mat4 placement = itemPlacement(instance.position, instance.rotation);
        generator.position = instance.position;
        if (collision != nullptr && (info.collisionFlags & 1U) == 0) {
            if (const auto floor = collision->floorAt(instance.position, 3.0f, 6.0f)) {
                // AddItemSub uses the same floor lift as other placed items.
                // Ground portals otherwise share the floor's depth exactly.
                generator.position.y = floor->y + ItemFigure::kFloorLift;
            }
        }
        generator.yaw = std::atan2(placement[2][0], placement[2][2]);
        generator.placement = placement;
        generator.placement[3] = Vec4{generator.position, 1};
        generator.direction = Vec3{placement[2][0], 0.0f, placement[2][2]};
        if (glm::length(generator.direction) > 0.001f) {
            generator.direction = glm::normalize(generator.direction);
        } else {
            generator.direction = Vec3{0.0f, 0.0f, 1.0f};
        }
        // SetItem copies the original offset before replacing a rat's info by LOW.
        generator.collisionOffset = info.collisionOffset;
        generator.targetRadius = body->radius;
        generator.targetHeight = body->height;
        generator.clearance = body->height;
        generator.patrolClearance = body->radius;
        generator.viewRadius = kGeneratorViewScale * std::max(info.radius, info.height);
        generator.alwaysActive = (info.activeType & 0x40U) != 0 || (instance.flags & 1U) != 0;
        generator.box.centre = generator.position;
        generator.box.yaw = generator.yaw;
        generator.box.halfAcross = body->xSize > 0.0f ? body->xSize : body->radius;
        generator.box.halfAlong = body->zSize > 0.0f ? body->zSize : body->radius;
        generator.box.height = body->height;
        generator.box.cylinderRadius = body->collisionType == 1 ? body->radius : 0.0f;
        generator.box.solid = generator.presence == Generator::Presence::Shown;
        bindSupport(generator, info, instance.position, authored ? &*authored : collision);
        generator.countdown = 0;
        m_generators.push_back(std::move(generator));
    }
    syncFloors();
    return true;
}

void Generators::bindSupport(Generator& generator, const ItemInfo& info, const Vec3& authored,
                             const WorldCollision* collision) {
    if (collision == nullptr || (info.collisionFlags & 1U) != 0) {
        return;
    }
    const auto floor = collision->floorAt(authored, 4, 10);
    if (!floor) {
        return;
    }
    const auto transform = collision->objectTransform(floor->object);
    if (!transform) {
        return;
    }
    generator.boxOffset = Vec3{glm::inverse(generator.placement) * Vec4{generator.box.centre, 1}};
    generator.boxYaw = generator.box.yaw - generator.yaw;
    Mat4 placement = generator.placement;
    placement[3] = Vec4{authored.x, floor->y + ItemFigure::kFloorLift, authored.z, 1};
    generator.support = floor->object;
    generator.supportLocal = glm::inverse(*transform) * placement;
}

void Generators::syncFloors() {
    if (m_collision == nullptr) {
        return;
    }
    for (Generator& generator : m_generators) {
        if (generator.support < 0) {
            continue;
        }
        const auto transform = m_collision->objectTransform(generator.support);
        if (!transform) {
            generator.support = -1;
            continue;
        }
        // Disabling a floor's collision does not detach its scenery children.
        generator.placement = *transform * generator.supportLocal;
        generator.position = Vec3{generator.placement[3]};
        generator.yaw = std::atan2(generator.placement[2].x, generator.placement[2].z);
        generator.direction = Vec3{std::sin(generator.yaw), 0, std::cos(generator.yaw)};
        generator.box.centre = Vec3{generator.placement * Vec4{generator.boxOffset, 1}};
        generator.box.yaw = generator.yaw + generator.boxYaw;
        if (generator.bossFigure != nullptr) {
            generator.bossFigure->placeAt(generator.placement);
        }
    }
}

bool Generators::placeBoss(RenderDevice& device, const ItemInfo& info, ItemArchive& items,
                           Enemies& enemies, s32 kind, const Mat4& placement,
                           const WorldCollision* collision, std::string_view tree, bool settled) {
    if (info.type != ItemInfo::kGenerator || info.name != "BOSSGEN" || kind < 0 ||
        kind >= kEnemyKindCount || !enemies.loadKind(kind)) {
        return false;
    }
    Generator generator;
    generator.boss = true;
    generator.kind = kind;
    generator.tier = 1;
    generator.state = 1;
    // The default instance has strength one and AI zero. Its forty-tick item
    // animation startup is separate from the brood counter, which starts ready.
    generator.algorithm = 0;
    generator.countdown = 0;
    generator.viewRadius = kGeneratorViewScale * std::max(info.radius, info.height);
    generator.alwaysActive = (info.activeType & 0x40U) != 0;
    generator.most = static_cast<s32>(static_cast<f32>(kDefaultMost[0]) * m_scales.most);
    generator.interval = static_cast<s32>(static_cast<f32>(kDefaultInterval[0]) * m_scales.rate);
    generator.health = generator.threshold =
        std::trunc(static_cast<f32>(info.hitPoints) * m_scales.health);
    generator.armor = static_cast<f32>(info.armor);
    ItemInstance instance;
    instance.position = Vec3{placement[3]};
    const f32 yaw = std::atan2(placement[2].x, placement[2].z);
    instance.rotation.y = -yaw;
    generator.bossFigure = std::make_unique<ItemFigure>();
    // AtreeMatchAnyHeader also searches the summoned species' loaded archive.
    // The Genie's BOSSGEN tree lives with WIND, not with DJINN or the level items.
    ItemArchive* art = &items;
    if (!items.trees.find(tree)) {
        if (ItemArchive* brood = enemies.archive(kind);
            brood != nullptr && brood->trees.find(tree)) {
            art = brood;
        }
    }
    // A missing BOSSGEN tree does not prevent PlaceItem from creating its gameplay object.
    generator.bossFigure->place(device, *art, tree, instance, collision);
    generator.bossFigure->play(0, !settled);
    if (settled) {
        // The projectile has already finished its landing before it leaves the generator.
        generator.bossFigure->update(static_cast<f32>(generator.bossFigure->ticksOf(0)) / 60.0f);
    }
    generator.position = generator.bossFigure->position();
    generator.placement = placement;
    generator.placement[3] = Vec4{generator.position, 1};
    generator.yaw = yaw;
    generator.direction = Vec3{std::sin(generator.yaw), 0, std::cos(generator.yaw)};
    generator.collisionOffset = info.collisionOffset;
    generator.targetRadius = info.radius;
    generator.targetHeight = info.height;
    generator.clearance = info.height;
    generator.patrolClearance = info.radius;
    generator.box = generator.bossFigure->obstacle(info);
    bindSupport(generator, info, instance.position, collision);
    if (collision != nullptr) {
        m_collision = collision;
    }
    m_generators.push_back(std::move(generator));
    syncFloors();
    return true;
}

void Generators::clear() {
    m_collision = nullptr;
    m_specialBirth = 0;
    m_generators.clear();
    for (auto& bodies : m_bodies) {
        for (TreeModel& model : bodies->models) {
            model.clear();
        }
    }
    m_bodies.clear();
}

void Generators::updatePresence(Generator& generator, bool seen) const {
    // do_items' minoff gate changes an existing item's presence offscreen. A hidden
    // placement encountered on screen is suppressed for the rest of the level.
    if (generator.presence == Generator::Presence::Hidden) {
        if (seen) {
            generator.presence = Generator::Presence::Suppressed;
        } else if (shownToParty(generator.minPlayers, m_players)) {
            generator.presence = Generator::Presence::Shown;
        }
    } else if (generator.presence == Generator::Presence::Shown && !seen &&
               !shownToParty(generator.minPlayers, m_players)) {
        generator.presence = Generator::Presence::Hidden;
    }
    generator.box.solid = generator.presence == Generator::Presence::Shown && generator.state > 0;
}

void Generators::applyBroodEvents(Enemies& enemies) {
    for (const auto& event : enemies.takeGeneratorEvents()) {
        if (event.generator < 0 || static_cast<usize>(event.generator) >= m_generators.size()) {
            continue;
        }
        auto& generator = m_generators[static_cast<usize>(event.generator)];
        switch (event.kind) {
        case EnemyGeneratorEvent::Kind::Born: ++generator.living; break;
        case EnemyGeneratorEvent::Kind::Detached:
            generator.living = std::max(generator.living - 1, 0);
            break;
        case EnemyGeneratorEvent::Kind::PatrolHit: generator.algorithm = 0; break;
        case EnemyGeneratorEvent::Kind::PatrolDetached:
            generator.algorithm = 0;
            generator.living = 0;
            break;
        }
    }
}

void Generators::update(s32 ticks, Enemies& enemies, std::span<const EnemyView> players,
                        std::span<const Obstacle> obstacles, bool timeStopped) {
    syncFloors();
    if (ticks <= 0) {
        return;
    }
    // Refresh the entire obstacle roster before any generator attempts a birth.
    for (Generator& generator : m_generators) {
        const bool seen =
            !m_view.has_value() || m_view->sees(generator.position, generator.viewRadius);
        updatePresence(generator, seen);
    }
    // uncouple_enemy releases quota before the death animation. A patrol's
    // departure resets the counter, which cannot be reconstructed by a census.
    applyBroodEvents(enemies);
    for (usize g = 0; g < m_generators.size(); ++g) {
        Generator& generator = m_generators[g];
        const bool seen =
            !m_view.has_value() || m_view->sees(generator.position, generator.viewRadius);
        if (generator.presence != Generator::Presence::Shown) {
            continue;
        }
        if (generator.bossFigure != nullptr && generator.state > 0) {
            generator.bossFigure->update(static_cast<f32>(ticks) / 60.0f);
        }
        if (generator.state <= 0 || generator.tier <= 0) {
            continue;
        }
        const bool patrol = generator.algorithm == kPatrolWay;
        if (!patrol && !seen && !generator.alwaysActive) {
            continue;
        }
        // do_items checks the living brood quota before generate_now ticks its timer.
        // A full brood freezes the remaining wait; a death frees a slot, not a free birth.
        // Patrol generators instead keep one sentry, with no ordinary quota or wait.
        if (patrol ? generator.living > 0
                   : generator.most <= 0 || generator.living >= generator.most) {
            continue;
        }
        if (!patrol && generator.countdown > 0) {
            generator.countdown -= ticks;
            continue;
        }
        if (timeStopped) {
            continue;
        }
        // Ordinary broods require a nearby player; a lone patrol sentry starts offscreen
        // as well, without waiting for the party to approach its post.
        bool near = false;
        for (const EnemyView& view : players) {
            near = near || (!view.hidden &&
                            glm::distance(view.position, generator.position) <= kActiveDistance);
        }
        if (!patrol && !near) {
            continue;
        }
        EnemySpawn spawn;
        spawn.kind = generator.kind;
        spawn.tier = generator.tier;
        spawn.algorithm = generator.algorithm;
        if (generator.kind == -2 || generator.kind == -3) {
            const usize at = m_specialBirth++ % kTempleKinds.size();
            spawn.kind = generator.kind == -2 ? kTempleKinds[at] : kHellKinds[at];
            spawn.tier = generator.kind == -2 ? 2 : 3;
            spawn.algorithm = generator.kind == -2 ? 7 : kHellAlgorithms[at];
            spawn.allBirthDirections = true;
        }
        spawn.position = generator.position;
        spawn.direction = generator.direction;
        spawn.clearance = patrol ? generator.patrolClearance : generator.clearance;
        spawn.patrolBirth = patrol;
        spawn.priority =
            seen || patrol ? EnemySpawn::Priority::Offscreen : EnemySpawn::Priority::FreeSlotOnly;
        spawn.generator = static_cast<s32>(g);
        if (generator.algorithm == kZigZagWay) {
            spawn.zigZagSide = (generator.bred & 1) == 0 ? 1 : -1;
        }
        std::vector<Obstacle> birthObstacles{obstacles.begin(), obstacles.end()};
        for (usize other = 0; other < m_generators.size(); ++other) {
            if (other != g && standing(static_cast<s32>(other))) {
                birthObstacles.push_back(m_generators[other].box);
            }
        }
        const auto child = enemies.spawn(spawn, players, birthObstacles);
        // Recycled slots release their previous generator even when birth fails.
        applyBroodEvents(enemies);
        if (!child.has_value()) {
            continue;
        }
        ++generator.bred;
        if (patrol) {
            continue;
        }
        // The next takes longer, the countdown stretched by a share that grows a birth at a
        // time and wraps.
        generator.countdown = static_cast<s32>(
            kCountdownScale * static_cast<f32>(generator.interval) * (1.0f + generator.ratio));
        generator.ratio += 1.0f / (2.0f * static_cast<f32>(generator.most));
        if (generator.ratio > 1.0f) {
            generator.ratio = 0.0f;
        }
    }
}

s32 Generators::stateFor(const Generator& generator, bool destroyed) {
    if (destroyed || generator.health <= 0.0f) {
        return 0;
    }
    if (generator.health <= generator.threshold) {
        return 1;
    }
    if (generator.health <= generator.threshold * 2.0f) {
        return 2;
    }
    return 3;
}

std::optional<GeneratorEvent> Generators::strike(s32 id, f32 power, [[maybe_unused]] s32 byPlayer) {
    if (id < 0 || static_cast<usize>(id) >= m_generators.size()) {
        return std::nullopt;
    }
    Generator& generator = m_generators[static_cast<usize>(id)];
    if (generator.state <= 0 || generator.presence != Generator::Presence::Shown ||
        generator.armor < 0.0f) {
        return std::nullopt;
    }
    f32 amount = power - generator.armor;
    if (amount <= 0.0f) {
        amount = 1.0f;
    }
    generator.health = std::max(generator.health - std::round(amount), 0.0f);
    const s32 state = stateFor(generator, false);
    const bool changed = state != generator.state;
    generator.state = state;
    // A crumbling state is also the strength it breeds at from then on, and it breeds twice
    // as many; a caster brood goes back to seeking (fn_8005C1DC).
    if (state != generator.tier) {
        generator.tier = state;
        if (state > 0) {
            generator.most *= 2;
        }
        if (std::ranges::find(kCasterAlgorithms, generator.algorithm) != kCasterAlgorithms.end()) {
            generator.algorithm = 0;
        }
    }
    GeneratorEvent event;
    event.generator = id;
    event.kind = generator.kind;
    event.state = state;
    event.position = generator.position;
    event.placement = generator.placement;
    event.stateChanged = changed;
    event.destroyed = state == 0;
    if (event.destroyed) {
        generator.box.solid = false;
    }
    return event;
}

std::optional<s32> Generators::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    std::optional<s32> best;
    f32 bestDistance = 0.0f;
    const Vec3 sweep = to - from;
    const f32 length = glm::length(sweep);
    for (usize g = 0; g < m_generators.size(); ++g) {
        const Generator& generator = m_generators[g];
        if (generator.state <= 0 || generator.presence != Generator::Presence::Shown) {
            continue;
        }
        const f32 reach = std::max(generator.box.halfAcross, generator.box.halfAlong);
        const Vec3 centre = generator.position + Vec3{0.0f, 0.5f * generator.box.height, 0.0f};
        const f32 t =
            length > 0.001f
                ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f)
                : 0.0f;
        const Vec3 nearest = from + sweep * t;
        if (flatDistance(nearest, centre) <= radius + reach &&
            std::abs(nearest.y - centre.y) <= radius + 0.5f * generator.box.height + 1.0f) {
            const f32 distance = glm::length(nearest - from);
            if (!best.has_value() || distance < bestDistance) {
                best = static_cast<s32>(g);
                bestDistance = distance;
            }
        }
    }
    return best;
}

std::vector<s32> Generators::within(const Vec3& centre, f32 radius) const {
    std::vector<s32> out;
    for (usize g = 0; g < m_generators.size(); ++g) {
        const Generator& generator = m_generators[g];
        if (generator.state <= 0 || generator.presence != Generator::Presence::Shown) {
            continue;
        }
        const f32 reach = std::max(generator.box.halfAcross, generator.box.halfAlong);
        if (glm::length(generator.position + Vec3{0.0f, 0.5f * generator.box.height, 0.0f} -
                        centre) <= radius + reach) {
            out.push_back(static_cast<s32>(g));
        }
    }
    return out;
}

std::vector<Obstacle> Generators::obstacles() const {
    std::vector<Obstacle> out;
    for (const Generator& generator : m_generators) {
        if (generator.state > 0 && generator.presence == Generator::Presence::Shown) {
            out.push_back(generator.box);
        }
    }
    return out;
}

std::vector<Obstacle> Generators::enemyObstacles() const {
    std::vector<Obstacle> out;
    for (const Generator& generator : m_generators) {
        if (generator.state > 0 && generator.presence == Generator::Presence::Shown &&
            generator.kind != kWormKind) {
            out.push_back(generator.box);
        }
    }
    return out;
}

void Generators::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const Generator& generator : m_generators) {
        if (generator.presence != Generator::Presence::Shown) {
            continue;
        }
        if (generator.boss) {
            if (generator.state > 0 && generator.bossFigure != nullptr) {
                generator.bossFigure->draw(device, clip, lighting);
            }
            continue;
        }
        const Bodies* bodies = bodiesOf(generator.kind);
        if (bodies == nullptr) {
            continue;
        }
        const TreeModel& model =
            bodies->models[static_cast<usize>(std::clamp(generator.state, 0, kStates))];
        if (!model.bound()) {
            continue;
        }
        model.draw(device, clip, generator.placement, lighting);
    }
}

bool Generators::standing(s32 id) const {
    return id >= 0 && static_cast<usize>(id) < m_generators.size() &&
           m_generators[static_cast<usize>(id)].state > 0 &&
           m_generators[static_cast<usize>(id)].presence == Generator::Presence::Shown;
}

bool Generators::bodyShown(s32 id) const {
    if (id < 0 || static_cast<usize>(id) >= m_generators.size()) {
        return false;
    }
    const Generator& generator = m_generators[static_cast<usize>(id)];
    if (generator.presence != Generator::Presence::Shown) {
        return false;
    }
    if (generator.boss) {
        return generator.state > 0 && generator.bossFigure != nullptr &&
               generator.bossFigure->hasFigure();
    }
    const Bodies* bodies = bodiesOf(generator.kind);
    return bodies != nullptr &&
           bodies->models[static_cast<usize>(std::clamp(generator.state, 0, kStates))].bound();
}

s32 Generators::stateOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].state;
}
f32 Generators::healthOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].health;
}
s32 Generators::kindOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].kind;
}
s32 Generators::tierOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].tier;
}
s32 Generators::algorithmOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].algorithm;
}
s32 Generators::mostOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].most;
}
s32 Generators::intervalOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].interval;
}
s32 Generators::countdownOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].countdown;
}
s32 Generators::bredOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].bred;
}
const Vec3& Generators::positionOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].position;
}
MissileTarget Generators::target(s32 index, s32 id) const {
    const auto& generator = m_generators.at(static_cast<usize>(index));
    MissileTarget result{id, generator.position,
                         std::max(generator.box.halfAcross, generator.box.halfAlong),
                         generator.box.height};
    result.acquisition =
        TargetAssist::itemAcquisition(generator.placement, generator.collisionOffset,
                                      generator.targetRadius, generator.targetHeight, 1);
    return result;
}

const Obstacle& Generators::boxOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].box;
}

} // namespace gdl::game
