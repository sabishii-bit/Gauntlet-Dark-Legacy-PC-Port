#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
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
    f32 health = 1.0f;
    f32 rate = 1.0f; ///< the difficulty gain already in it
    f32 most = 1.0f;
};

/** A generator struck: the state it is now in (three whole, down to nought, destroyed). */
struct GeneratorEvent {
    s32 generator = -1;
    s32 kind = 0;
    s32 state = 3;
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
    static constexpr s32 kStates = 3;
    static constexpr f32 kCountdownScale = 6.0f; ///< ticks a unit of interval counts for
    static constexpr f32 kNearDistance = 48.0f;  ///< a player this close makes one breed
    static constexpr std::array<s32, 3> kDefaultMost{10, 5, 2};
    static constexpr std::array<s32, 3> kDefaultInterval{5, 10, 15};

    /** Stands the layout's generators for a party of `players`, each breeding the kind the
     * level's `roster` gives for the one its record names; their kinds' archives are loaded
     * into `enemies`. Instances naming a kind that is not known are left out. */
    bool bind(RenderDevice& device, const WorldLayout& layout, Enemies& enemies,
              const WorldCollision* collision, const GeneratorScales& scales, s32 players,
              std::span<const LevelEnemy> roster = {});
    void clear();
    /** A boss effect leaves a tier-one generator. Its optional BOSSGEN art is borrowed. */
    bool placeBoss(RenderDevice& device, const ItemInfo& info, ItemArchive& items, Enemies& enemies,
                   s32 kind, const Mat4& placement, const WorldCollision* collision);

    /** Runs the countdowns, breeding into `enemies` where a player is within reach. */
    void update(s32 ticks, Enemies& enemies, std::span<const EnemyView> players,
                std::span<const Obstacle> obstacles = {});

    /** Strikes a generator with `power`; what comes of it, if its state changed. */
    std::optional<GeneratorEvent> strike(s32 id, f32 power, s32 byPlayer);
    /** The nearest standing generator a sweep touches. */
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    std::vector<s32> within(const Vec3& centre, f32 radius) const;
    /** The standing generators' boxes. */
    std::vector<Obstacle> obstacles() const;

    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

    usize count() const { return m_generators.size(); }
    bool standing(s32 id) const;
    /** Whether the generator has a body to show in its state. */
    bool bodyShown(s32 id) const;
    s32 stateOf(s32 id) const;
    f32 healthOf(s32 id) const;
    s32 kindOf(s32 id) const;
    s32 tierOf(s32 id) const;
    s32 mostOf(s32 id) const;
    s32 intervalOf(s32 id) const;
    s32 countdownOf(s32 id) const;
    s32 bredOf(s32 id) const;
    const Vec3& positionOf(s32 id) const;
    const Obstacle& boxOf(s32 id) const;
    /** The generator's parameters as a level's record gives them, for tests and tools. */
    static s32 paramOf(const ItemInstance& instance, usize index);

private:
    struct Bodies {
        s32 kind = -1;
        std::array<TreeInfo, kStates + 1> trees; ///< one node each, the state's object
        std::array<TreeModel, kStates + 1> models;
    };

    struct Generator {
        s32 kind = 0;
        s32 tier = 1;
        s32 algorithm = -1;
        s32 most = 0;
        s32 interval = 0;
        f32 health = 0.0f;
        f32 threshold = 0.0f;
        f32 armor = 0.0f;
        s32 state = kStates;
        s32 countdown = 0;
        f32 ratio = 0.0f; ///< grows each birth, stretching the countdown
        s32 bred = 0;
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 yaw = 0.0f;
        Vec3 direction{0.0f, 0.0f, 1.0f};
        f32 clearance = 0.0f;
        Obstacle box;
        std::unique_ptr<ItemFigure> bossFigure;
        bool boss = false;
    };

    Bodies* bodiesOf(s32 kind);
    const Bodies* bodiesOf(s32 kind) const;
    bool loadBodies(RenderDevice& device, Enemies& enemies, s32 kind);
    static s32 stateFor(const Generator& generator, bool destroyed);

    std::vector<Generator> m_generators;
    std::vector<std::unique_ptr<Bodies>> m_bodies;
    GeneratorScales m_scales;
};

} // namespace gdl::game
