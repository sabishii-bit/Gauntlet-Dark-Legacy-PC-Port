#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/Enemies.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {

/** The level's scales on its generators. */
struct GeneratorScales {
    float health = 1.0f;
    float rate = 1.0f; ///< the difficulty gain already in it
    float most = 1.0f;
};

/** A generator struck: the state it is now in (three whole, down to nought, destroyed). */
struct GeneratorEvent {
    std::int32_t generator = -1;
    std::int32_t kind = 0;
    std::int32_t state = 3;
    Vec3 position{0.0f, 0.0f, 0.0f};
    bool destroyed = false;
};

/**
 * The level's generators: the huts and pits that breed the swarm. Each holds a kind and a
 * strength (the tier it breeds and how much it can take), keeps up to its count of enemies
 * out at once, and breeds another as its countdown runs out while a player is near. Struck
 * enough it crumbles a state at a time to nothing, freeing whatever it bred.
 */
class Generators {
public:
    static constexpr std::int32_t kStates = 3;
    static constexpr float kCountdownScale = 6.0f; ///< ticks a unit of interval counts for
    static constexpr float kNearDistance = 48.0f;  ///< a player this close makes one breed
    static constexpr std::array<std::int32_t, 3> kDefaultMost{10, 5, 2};
    static constexpr std::array<std::int32_t, 3> kDefaultInterval{5, 10, 15};

    /** Stands the layout's generators for a party of `players`, each breeding the kind the
     * level's `roster` gives for the one its record names; their kinds' archives are loaded
     * into `enemies`. Instances naming a kind that is not known are left out. */
    bool bind(RenderDevice& device, const WorldLayout& layout, Enemies& enemies,
              const WorldCollision* collision, const GeneratorScales& scales, std::int32_t players,
              std::span<const LevelEnemy> roster = {});
    void clear();

    /** Runs the countdowns, breeding into `enemies` where a player is within reach. */
    void update(std::int32_t ticks, Enemies& enemies, std::span<const EnemyView> players,
                std::span<const Obstacle> obstacles = {});

    /** Strikes a generator with `power`; what comes of it, if its state changed. */
    std::optional<GeneratorEvent> strike(std::int32_t id, float power, std::int32_t byPlayer);
    /** The nearest standing generator a sweep touches. */
    std::optional<std::int32_t> struckBy(const Vec3& from, const Vec3& to, float radius) const;
    std::vector<std::int32_t> within(const Vec3& centre, float radius) const;
    /** The standing generators' boxes. */
    std::vector<Obstacle> obstacles() const;

    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

    std::size_t count() const { return m_generators.size(); }
    bool standing(std::int32_t id) const;
    /** Whether the generator has a body to show in its state. */
    bool bodyShown(std::int32_t id) const;
    std::int32_t stateOf(std::int32_t id) const;
    float healthOf(std::int32_t id) const;
    std::int32_t kindOf(std::int32_t id) const;
    std::int32_t tierOf(std::int32_t id) const;
    std::int32_t mostOf(std::int32_t id) const;
    std::int32_t intervalOf(std::int32_t id) const;
    std::int32_t countdownOf(std::int32_t id) const;
    std::int32_t bredOf(std::int32_t id) const;
    const Vec3& positionOf(std::int32_t id) const;
    const Obstacle& boxOf(std::int32_t id) const;
    /** The generator's parameters as a level's record gives them, for tests and tools. */
    static std::int32_t paramOf(const ItemInstance& instance, std::size_t index);

private:
    struct Bodies {
        std::int32_t kind = -1;
        std::array<TreeInfo, kStates + 1> trees; ///< one node each, the state's object
        std::array<TreeModel, kStates + 1> models;
    };

    struct Generator {
        std::int32_t kind = 0;
        std::int32_t tier = 1;
        std::int32_t algorithm = -1;
        std::int32_t most = 0;
        std::int32_t interval = 0;
        float health = 0.0f;
        float threshold = 0.0f;
        float armor = 0.0f;
        std::int32_t state = kStates;
        std::int32_t countdown = 0;
        float ratio = 0.0f; ///< grows each birth, stretching the countdown
        std::int32_t bred = 0;
        Vec3 position{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        Vec3 direction{0.0f, 0.0f, 1.0f};
        float clearance = 0.0f;
        Obstacle box;
    };

    Bodies* bodiesOf(std::int32_t kind);
    const Bodies* bodiesOf(std::int32_t kind) const;
    bool loadBodies(RenderDevice& device, Enemies& enemies, std::int32_t kind);
    static std::int32_t stateFor(const Generator& generator, bool destroyed);

    std::vector<Generator> m_generators;
    std::vector<std::unique_ptr<Bodies>> m_bodies;
};

} // namespace gdl::game
