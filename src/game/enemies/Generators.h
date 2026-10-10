#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/Enemies.h"
#include "game/world/ItemFigure.h"
#include "game/world/PlayerMissiles.h"

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
    Mat4 placement{1};
    bool stateChanged = false;
    bool destroyed = false;
};

/** What a player's blow on a generator of `kind` (the kind it breeds) earns
 * (PlayerDamagedItem, combat.c 338): five times its row of the original's tables, the
 * destroying blow's (lbl_8011BBA8) or a hit's (lbl_8011BB20); the unknown kinds -2 and -3
 * count as the second and third, any other below nought as the first. */
s32 generatorExperience(s32 kind, bool destroyed);

/**
 * The level's generators: the huts and pits that breed the swarm. Each holds a kind and a
 * strength (the tier it breeds and how much it can take), keeps up to its count of enemies
 * out at once, and breeds another as its countdown runs out while a player is near. Struck
 * enough it crumbles a state at a time to nothing, freeing whatever it bred.
 */
class Generators {
public:
    static constexpr s32 kStates = 3;
    static constexpr f32 kCountdownScale = 6.0f;    ///< ticks a unit of interval counts for
    static constexpr f32 kActiveDistance = 1000.0f; ///< no player this near, none breeds
    static constexpr std::array<s32, 3> kDefaultMost{10, 5, 2};
    static constexpr std::array<s32, 3> kDefaultInterval{5, 10, 15};

    /** Stands the layout's generators for a party of `players`, each breeding the kind the
     * level's `roster` gives for the one its record names; their kinds' archives are loaded
     * into `enemies`. Instances naming a kind that is not known are left out. */
    bool bind(RenderDevice& device, const WorldLayout& layout, Enemies& enemies,
              const WorldCollision* collision, const GeneratorScales& scales, s32 players,
              std::span<const LevelEnemy> roster = {}, s32 realm = -1,
              ItemArchive* realmItems = nullptr, std::span<TextureSet* const> lenders = {});
    void clear();
    /** Carries bodies, contact boxes and birth origins with their supporting world nodes. */
    void syncFloors();
    /** A boss effect leaves a tier-one generator. Its optional BOSSGEN art is borrowed. */
    bool placeBoss(RenderDevice& device, const ItemInfo& info, ItemArchive& items, Enemies& enemies,
                   s32 kind, const Mat4& placement, const WorldCollision* collision,
                   std::string_view tree = "BOSSGEN", bool settled = false);

    /** Ordinary broods update inside this view; patrol and always-active posts are exempt.
     * The normal level camera also limits attention distance; boss cameras omit it.
     * No view means every generator is visible. */
    void setView(std::optional<ViewVolume> view, std::optional<Vec3> attention = std::nullopt) {
        m_view = view;
        m_attention = attention;
    }
    /** Joined participants, including those waiting in the tower but not those who quit. */
    void setPlayerCount(s32 players) { m_players = players; }
    /** Obstacles supplied here are fixtures; the other generators are included internally. */
    void update(s32 ticks, Enemies& enemies, std::span<const EnemyView> players,
                std::span<const Obstacle> obstacles = {}, bool timeStopped = false);

    /** Reports each damaging strike; only state changes launch debris effects. */
    std::optional<GeneratorEvent> strike(s32 id, f32 power, s32 byPlayer);
    /** The nearest standing generator a sweep touches. */
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    std::vector<s32> within(const Vec3& centre, f32 radius) const;
    /** The standing generators' boxes. */
    std::vector<Obstacle> obstacles() const;
    /** Worm pits allow enemies to walk across, but remain targets and block other births. */
    std::vector<Obstacle> enemyObstacles() const;

    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 presentationAlpha = -1.0f);

    usize count() const { return m_generators.size(); }
    bool standing(s32 id) const;
    /** Whether the generator has a body to show in its state. */
    bool bodyShown(s32 id) const;
    /** Loaded art and visible draw records only; neither API runs brood logic. */
    struct BodyResource {
        s32 kind = 0;
        s32 state = 0;
        const TreeInfo* tree = nullptr;
        const TreeModel* model = nullptr;
        const TextureAnimator* textures = nullptr;
    };
    struct Presentation {
        usize index = 0;
        s32 kind = 0;
        s32 state = 0;
        Mat4 placement{1};
        f32 textureClock = 0;
        const ItemFigure* bossFigure = nullptr;
    };
    std::vector<BodyResource> bodyResources() const;
    std::vector<Presentation> presentation() const;
    s32 stateOf(s32 id) const;
    f32 healthOf(s32 id) const;
    s32 kindOf(s32 id) const;
    s32 tierOf(s32 id) const;
    s32 algorithmOf(s32 id) const;
    s32 mostOf(s32 id) const;
    s32 intervalOf(s32 id) const;
    s32 countdownOf(s32 id) const;
    s32 bredOf(s32 id) const;
    s32 livingOf(s32 id) const { return m_generators[static_cast<usize>(id)].living; }
    const Vec3& positionOf(s32 id) const;
    const Obstacle& boxOf(s32 id) const;
    MissileTarget target(s32 index, s32 id) const;
    /** The generator's parameters as a level's record gives them, for tests and tools. */
    static s32 paramOf(const ItemInstance& instance, usize index);

private:
    struct Bodies {
        s32 kind = -1;
        std::array<TreeInfo, kStates + 1> trees; ///< authored tree or single state object
        std::array<TreeModel, kStates + 1> models;
        TextureAnimator textures;
    };

    struct Generator {
        enum class Presence : u8 { Shown, Hidden, Suppressed };
        s32 minPlayers = 0;
        Presence presence = Presence::Shown;
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
        s32 living = 0; ///< retail quota counter, reset when a patrol offspring detaches
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 collisionOffset{0};
        f32 targetRadius = 0;
        f32 targetHeight = 0;
        Mat4 placement{1};
        f32 yaw = 0.0f;
        Vec3 direction{0.0f, 0.0f, 1.0f};
        f32 clearance = 0.0f;
        f32 patrolClearance = 0.0f;
        f32 viewRadius = 0.0f;     ///< how far outside the view it still counts as on screen
        bool alwaysActive = false; ///< authored offscreen updates, independent of visibility
        Obstacle box;
        s32 support = -1;
        Mat4 supportLocal{1};
        Vec3 boxOffset{0};
        f32 boxYaw = 0;
        std::unique_ptr<ItemFigure> bossFigure;
        bool boss = false;
    };

    Bodies* bodiesOf(s32 kind);
    const Bodies* bodiesOf(s32 kind) const;
    bool loadBodies(RenderDevice& device, Enemies& enemies, s32 kind, ItemArchive* realmItems,
                    std::span<TextureSet* const> lenders);
    static s32 stateFor(const Generator& generator, bool destroyed);
    bool seen(const Generator& generator) const;
    void updatePresence(Generator& generator, bool seen) const;
    void applyBroodEvents(Enemies& enemies);
    static void bindSupport(Generator& generator, const ItemInfo& info, const Vec3& authored,
                            const WorldCollision* collision);

    const WorldCollision* m_collision = nullptr;
    std::vector<Generator> m_generators;
    std::optional<ViewVolume> m_view;
    std::optional<Vec3> m_attention;
    std::vector<std::unique_ptr<Bodies>> m_bodies;
    GeneratorScales m_scales;
    s32 m_players = 1;
    u32 m_specialBirth = 0; ///< shared round-robin cursor, as in retail's enemy spawner
};

} // namespace gdl::game
