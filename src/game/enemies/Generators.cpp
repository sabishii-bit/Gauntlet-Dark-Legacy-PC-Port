#include "game/enemies/Generators.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

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

bool Generators::loadBodies(RenderDevice& device, Enemies& enemies, s32 kind) {
    if (bodiesOf(kind) != nullptr) {
        return true;
    }
    if (!enemies.loadKind(kind)) {
        return false;
    }
    ItemArchive* archive = enemies.archive(kind);
    if (archive == nullptr) {
        return false;
    }
    auto bodies = std::make_unique<Bodies>();
    bodies->kind = kind;
    const EnemyKind& info = enemyKind(kind);
    // The state's object: "GEN_GRU3", tried with the level-one and root suffixes as the
    // original does. Whole is three; nought is the ruin.
    for (s32 state = 0; state <= kStates; ++state) {
        const std::string base = std::format("GEN_{}{}", info.prefix, state);
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
                      std::span<const LevelEnemy> roster) {
    clear();
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    for (const ItemInstance& instance : layout.itemInstances()) {
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kGenerator || !shownToParty(instance.minPlayers, players)) {
            continue;
        }
        const auto named = enemyKindOf(info.name);
        if (!named.has_value()) {
            continue;
        }
        const s32 strength = std::max(paramOf(instance, 0), 1);
        const s32 kind = levelKindOf(roster, *named, strength);
        if (!loadBodies(device, enemies, kind)) {
            continue;
        }
        Generator generator;
        generator.kind = kind;
        generator.tier = std::clamp(strength, 1, 3);
        generator.algorithm = paramOf(instance, 1);
        if (generator.algorithm < 0) {
            generator.algorithm = enemyKind(kind).algorithm;
        }
        const auto tierIndex = static_cast<usize>(generator.tier - 1);
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
        generator.threshold = static_cast<f32>(info.hitPoints) * scales.health;
        generator.health = static_cast<f32>(info.hitPoints * generator.tier) * scales.health;
        generator.armor =
            info.armor > 0 ? static_cast<f32>(info.armor) : enemyKind(kind).generatorArmor;
        const Mat4 placement = itemPlacement(instance.position, instance.rotation);
        generator.position = instance.position;
        if (collision != nullptr) {
            if (const auto floor = collision->floorAt(instance.position, 3.0f, 6.0f)) {
                generator.position.y = floor->y;
            }
        }
        generator.yaw = std::atan2(placement[2][0], placement[2][2]);
        generator.direction = Vec3{placement[2][0], 0.0f, placement[2][2]};
        if (glm::length(generator.direction) > 0.001f) {
            generator.direction = glm::normalize(generator.direction);
        } else {
            generator.direction = Vec3{0.0f, 0.0f, 1.0f};
        }
        generator.clearance = info.height;
        generator.box.centre = generator.position;
        generator.box.yaw = generator.yaw;
        generator.box.halfAcross = info.xSize > 0.0f ? info.xSize : info.radius;
        generator.box.halfAlong = info.zSize > 0.0f ? info.zSize : info.radius;
        generator.box.height = info.height;
        generator.countdown = 0;
        m_generators.push_back(generator);
    }
    return true;
}

void Generators::clear() {
    m_generators.clear();
    for (auto& bodies : m_bodies) {
        for (TreeModel& model : bodies->models) {
            model.clear();
        }
    }
    m_bodies.clear();
}

void Generators::update(s32 ticks, Enemies& enemies, std::span<const EnemyView> players,
                        std::span<const Obstacle> obstacles) {
    if (ticks <= 0) {
        return;
    }
    // How many of each generator's are still about.
    std::vector<s32> out(m_generators.size(), 0);
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (!enemies.alive(id) && !enemies.dying(id)) {
            continue;
        }
        const s32 generator = enemies.generatorOf(id);
        if (generator >= 0 && static_cast<usize>(generator) < out.size()) {
            ++out[static_cast<usize>(generator)];
        }
    }
    for (usize g = 0; g < m_generators.size(); ++g) {
        Generator& generator = m_generators[g];
        if (generator.state <= 0 || generator.tier <= 0 || generator.most <= 0) {
            continue;
        }
        if (generator.countdown > 0) {
            generator.countdown -= ticks;
            continue;
        }
        if (out[g] >= generator.most) {
            continue;
        }
        bool near = false;
        for (const EnemyView& view : players) {
            near = near || (!view.hidden &&
                            flatDistance(view.position, generator.position) <= kNearDistance);
        }
        if (!near) {
            continue;
        }
        EnemySpawn spawn;
        spawn.kind = generator.kind;
        spawn.tier = generator.tier;
        spawn.algorithm = generator.algorithm;
        spawn.position = generator.position;
        spawn.direction = generator.direction;
        spawn.clearance = generator.clearance;
        spawn.generator = static_cast<s32>(g);
        if (!enemies.spawn(spawn, players, obstacles).has_value()) {
            continue;
        }
        ++generator.bred;
        ++out[g];
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

std::optional<GeneratorEvent> Generators::strike(s32 id, f32 power, s32 byPlayer) {
    if (id < 0 || static_cast<usize>(id) >= m_generators.size()) {
        return std::nullopt;
    }
    Generator& generator = m_generators[static_cast<usize>(id)];
    if (generator.state <= 0) {
        return std::nullopt;
    }
    const f32 amount = std::max(power - generator.armor, byPlayer >= 0 ? 1.0f : 0.0f);
    if (amount <= 0.0f) {
        return std::nullopt;
    }
    generator.health -= amount;
    const s32 state = stateFor(generator, false);
    if (state == generator.state) {
        return std::nullopt;
    }
    generator.state = state;
    GeneratorEvent event;
    event.generator = id;
    event.kind = generator.kind;
    event.state = state;
    event.position = generator.position;
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
        if (generator.state <= 0) {
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
        if (generator.state <= 0) {
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
        if (generator.state > 0) {
            out.push_back(generator.box);
        }
    }
    return out;
}

void Generators::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const Generator& generator : m_generators) {
        const Bodies* bodies = bodiesOf(generator.kind);
        if (bodies == nullptr) {
            continue;
        }
        const TreeModel& model =
            bodies->models[static_cast<usize>(std::clamp(generator.state, 0, kStates))];
        if (!model.bound()) {
            continue;
        }
        const Mat4 place = glm::rotate(glm::translate(Mat4{1.0f}, generator.position),
                                       generator.yaw, Vec3{0.0f, 1.0f, 0.0f});
        model.draw(device, clip, place, lighting);
    }
}

bool Generators::standing(s32 id) const {
    return id >= 0 && static_cast<usize>(id) < m_generators.size() &&
           m_generators[static_cast<usize>(id)].state > 0;
}

bool Generators::bodyShown(s32 id) const {
    if (id < 0 || static_cast<usize>(id) >= m_generators.size()) {
        return false;
    }
    const Generator& generator = m_generators[static_cast<usize>(id)];
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
const Obstacle& Generators::boxOf(s32 id) const {
    return m_generators[static_cast<usize>(id)].box;
}

} // namespace gdl::game
