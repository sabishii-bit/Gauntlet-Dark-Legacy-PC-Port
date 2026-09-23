#include "game/enemies/Generators.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>

namespace gdl::game {

namespace {

float flatDistance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

int Generators::paramOf(const ItemInstance& instance, std::size_t index) {
    if (index * 2 + 1 >= instance.params.size()) {
        return 0;
    }
    std::int16_t value = 0;
    std::memcpy(&value, &instance.params[index * 2], sizeof(value));
    return value;
}

Generators::Bodies* Generators::bodiesOf(int kind) {
    for (auto& bodies : m_bodies) {
        if (bodies->kind == kind) {
            return bodies.get();
        }
    }
    return nullptr;
}

const Generators::Bodies* Generators::bodiesOf(int kind) const {
    for (const auto& bodies : m_bodies) {
        if (bodies->kind == kind) {
            return bodies.get();
        }
    }
    return nullptr;
}

bool Generators::loadBodies(RenderDevice& device, Enemies& enemies, int kind) {
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
    for (int state = 0; state <= kStates; ++state) {
        const std::string base = std::format("GEN_{}{}", info.prefix, state);
        for (const char* suffix : {"L1", "", "ROOT"}) {
            const auto object = archive->models.find(base + suffix);
            if (!object.has_value()) {
                continue;
            }
            const unsigned int objectIndex = *object;
            TreeInfo& tree = bodies->trees[static_cast<std::size_t>(state)];
            tree.name = base;
            TreeNodeInfo node;
            node.name = base;
            node.object = archive->models.entry(objectIndex).name;
            tree.nodes.push_back(node);
            bodies->models[static_cast<std::size_t>(state)].bind(tree, archive->models,
                                                                 archive->textures, device);
            break;
        }
    }
    m_bodies.push_back(std::move(bodies));
    return true;
}

bool Generators::bind(RenderDevice& device, const WorldLayout& layout, Enemies& enemies,
                      const WorldCollision* collision, const GeneratorScales& scales, int players,
                      std::span<const LevelEnemy> roster) {
    clear();
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    for (const ItemInstance& instance : layout.itemInstances()) {
        if (instance.info < 0 || static_cast<std::size_t>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<std::size_t>(instance.info)];
        if (info.type != ItemInfo::kGenerator || !shownToParty(instance.minPlayers, players)) {
            continue;
        }
        const auto named = enemyKindOf(info.name);
        if (!named.has_value()) {
            continue;
        }
        const int strength = std::max(paramOf(instance, 0), 1);
        const int kind = levelKindOf(roster, *named, strength);
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
        const auto tierIndex = static_cast<std::size_t>(generator.tier - 1);
        int most = paramOf(instance, 2);
        int interval = paramOf(instance, 3);
        if (most == 0) {
            most = kDefaultMost[tierIndex];
        }
        if (interval == 0) {
            interval = kDefaultInterval[tierIndex];
        }
        // Scaled the way the original truncates them: to a whole count and interval.
        generator.most = static_cast<int>(static_cast<float>(most) * scales.most);
        generator.interval = static_cast<int>(static_cast<float>(interval) * scales.rate);
        generator.threshold = static_cast<float>(info.hitPoints) * scales.health;
        generator.health = static_cast<float>(info.hitPoints * generator.tier) * scales.health;
        generator.armor =
            info.armor > 0 ? static_cast<float>(info.armor) : enemyKind(kind).generatorArmor;
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

void Generators::update(int ticks, Enemies& enemies, std::span<const EnemyView> players,
                        std::span<const Obstacle> obstacles) {
    if (ticks <= 0) {
        return;
    }
    // How many of each generator's are still about.
    std::vector<int> out(m_generators.size(), 0);
    for (int id = 0; id < Enemies::kMost; ++id) {
        if (!enemies.alive(id) && !enemies.dying(id)) {
            continue;
        }
        const int generator = enemies.generatorOf(id);
        if (generator >= 0 && static_cast<std::size_t>(generator) < out.size()) {
            ++out[static_cast<std::size_t>(generator)];
        }
    }
    for (std::size_t g = 0; g < m_generators.size(); ++g) {
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
        spawn.generator = static_cast<int>(g);
        if (!enemies.spawn(spawn, players, obstacles).has_value()) {
            continue;
        }
        ++generator.bred;
        ++out[g];
        // The next takes longer, the countdown stretched by a share that grows a birth at a
        // time and wraps.
        generator.countdown = static_cast<int>(
            kCountdownScale * static_cast<float>(generator.interval) * (1.0f + generator.ratio));
        generator.ratio += 1.0f / (2.0f * static_cast<float>(generator.most));
        if (generator.ratio > 1.0f) {
            generator.ratio = 0.0f;
        }
    }
}

int Generators::stateFor(const Generator& generator, bool destroyed) {
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

std::optional<GeneratorEvent> Generators::strike(int id, float power, int byPlayer) {
    if (id < 0 || static_cast<std::size_t>(id) >= m_generators.size()) {
        return std::nullopt;
    }
    Generator& generator = m_generators[static_cast<std::size_t>(id)];
    if (generator.state <= 0) {
        return std::nullopt;
    }
    const float amount = std::max(power - generator.armor, byPlayer >= 0 ? 1.0f : 0.0f);
    if (amount <= 0.0f) {
        return std::nullopt;
    }
    generator.health -= amount;
    const int state = stateFor(generator, false);
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

std::optional<int> Generators::struckBy(const Vec3& from, const Vec3& to, float radius) const {
    std::optional<int> best;
    float bestDistance = 0.0f;
    const Vec3 sweep = to - from;
    const float length = glm::length(sweep);
    for (std::size_t g = 0; g < m_generators.size(); ++g) {
        const Generator& generator = m_generators[g];
        if (generator.state <= 0) {
            continue;
        }
        const float reach = std::max(generator.box.halfAcross, generator.box.halfAlong);
        const Vec3 centre = generator.position + Vec3{0.0f, 0.5f * generator.box.height, 0.0f};
        const float t =
            length > 0.001f
                ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f)
                : 0.0f;
        const Vec3 nearest = from + sweep * t;
        if (flatDistance(nearest, centre) <= radius + reach &&
            std::abs(nearest.y - centre.y) <= radius + 0.5f * generator.box.height + 1.0f) {
            const float distance = glm::length(nearest - from);
            if (!best.has_value() || distance < bestDistance) {
                best = static_cast<int>(g);
                bestDistance = distance;
            }
        }
    }
    return best;
}

std::vector<int> Generators::within(const Vec3& centre, float radius) const {
    std::vector<int> out;
    for (std::size_t g = 0; g < m_generators.size(); ++g) {
        const Generator& generator = m_generators[g];
        if (generator.state <= 0) {
            continue;
        }
        const float reach = std::max(generator.box.halfAcross, generator.box.halfAlong);
        if (glm::length(generator.position + Vec3{0.0f, 0.5f * generator.box.height, 0.0f} -
                        centre) <= radius + reach) {
            out.push_back(static_cast<int>(g));
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
            bodies->models[static_cast<std::size_t>(std::clamp(generator.state, 0, kStates))];
        if (!model.bound()) {
            continue;
        }
        const Mat4 place = glm::rotate(glm::translate(Mat4{1.0f}, generator.position),
                                       generator.yaw, Vec3{0.0f, 1.0f, 0.0f});
        model.draw(device, clip, place, lighting);
    }
}

bool Generators::standing(int id) const {
    return id >= 0 && static_cast<std::size_t>(id) < m_generators.size() &&
           m_generators[static_cast<std::size_t>(id)].state > 0;
}

bool Generators::bodyShown(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= m_generators.size()) {
        return false;
    }
    const Generator& generator = m_generators[static_cast<std::size_t>(id)];
    const Bodies* bodies = bodiesOf(generator.kind);
    return bodies != nullptr &&
           bodies->models[static_cast<std::size_t>(std::clamp(generator.state, 0, kStates))]
               .bound();
}

int Generators::stateOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].state;
}
float Generators::healthOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].health;
}
int Generators::kindOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].kind;
}
int Generators::tierOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].tier;
}
int Generators::mostOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].most;
}
int Generators::intervalOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].interval;
}
int Generators::countdownOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].countdown;
}
int Generators::bredOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].bred;
}
const Vec3& Generators::positionOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].position;
}
const Obstacle& Generators::boxOf(int id) const {
    return m_generators[static_cast<std::size_t>(id)].box;
}

} // namespace gdl::game
