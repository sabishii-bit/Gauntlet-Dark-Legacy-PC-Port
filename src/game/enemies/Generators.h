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

#include "game/combat/MissileTarget.h"
#include "game/enemies/Enemies.h"
#include "game/level/ImportedGenerators.h"
#include "game/level/ItemFigure.h"
#include "game/mechanics/SwarmRules.h"

namespace gdl::game {

class NativeStateResources;

/** The level's scales on its generators. */
struct GeneratorScales {
    f32 health = 1.0f;
    f32 rate = 1.0f; ///< the difficulty gain already in it
    f32 most = 1.0f;
    bool operator==(const GeneratorScales&) const = default;
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
    u64 instance = 0; ///< source lifetime, retained even by a destroying strike
    bool operator==(const GeneratorEvent&) const = default;
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
    Generators() = default;
    Generators(const Generators&) = delete;
    Generators& operator=(const Generators&) = delete;
    Generators(Generators&&) = delete;
    Generators& operator=(Generators&&) = delete;
    ~Generators() = default;

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
    /** The kinds of the temple's and the underworld's mixed broods: generators that breed
     * their realm's four kinds in turn, at their realm's strength. */
    static constexpr s32 kTempleBrood = -2;
    static constexpr s32 kHellBrood = -3;
    /** Stands a generator of `kind` at `position` facing `yaw`, breeding at `tier`, on the
     * level's record for the kind (any of its generator records failing that, else stock
     * measures), for the developer console: its id, or nothing without bodies or a slot. A
     * mixed brood breeds at its realm's strength, `tier` then only its durability; its bodies
     * are in `realmItems` (the level in play's item archive when null) and its record is
     * `record` when given. */
    std::optional<s32> spawn(RenderDevice& device, Enemies& enemies, s32 kind, s32 tier,
                             const Vec3& position, f32 yaw, ItemArchive* realmItems = nullptr,
                             const ItemInfo* record = nullptr);
    /** Installs an imported native placement into the normal GDL brood and damage
     * paths. Its level owns the artwork; only this pool slot's default art is hidden. */
    std::optional<s32> spawnImported(RenderDevice& device, Enemies& enemies,
                                     const ImportedGenerator& imported, f32 difficultyGain = 1);
    bool externallyPresented(s32 id) const;
    /** A boss effect leaves a tier-one generator. Its optional BOSSGEN art is borrowed. */
    bool placeBoss(RenderDevice& device, const ItemInfo& info, ItemArchive& items, Enemies& enemies,
                   s32 kind, const Mat4& placement, const WorldCollision* collision,
                   std::string_view tree = "BOSSGEN", bool settled = false);

    /** Ordinary broods update inside this view; patrol and always-active posts are exempt.
     * The normal level camera also limits attention distance; boss cameras omit it.
     * No view means every generator is visible. */
    void setView(std::optional<ViewVolume> view, std::optional<Vec3> attention = std::nullopt) {
        m_sim.view =
            view ? std::optional{ViewSet::of(*view, attention.value_or(Vec3{0.0f}))} : std::nullopt;
        m_sim.attentionGate = attention.has_value();
    }
    /** What the party's cameras take in together, each with the point it looks at; with
     * `attentionGate` a generator must also stand near a camera's point. */
    void setViews(const std::optional<ViewSet>& views, bool attentionGate) {
        m_sim.view = views;
        m_sim.attentionGate = attentionGate;
    }
    /** Joined participants, including those waiting in the tower but not those who quit. */
    void setPlayerCount(s32 players) { m_sim.players = players; }
    /** The mechanics' tuning of the broods, borrowed until close; the original's when null:
     * how much sooner they breed, how many more they keep out, and whether a hut near the
     * party breeds past its brood. Read afresh each update, never kept. */
    void setRules(const SwarmRules* swarm) { m_swarm = swarm; }
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
    struct Contact {
        u32 slot = 0;
        Obstacle box;
    };
    /** Same native order and visibility as obstacles, retaining append-only
     * pool slots for a caller's epoch-scoped collision replay. */
    std::vector<Contact> identifiedObstacles() const;
    /** Worm pits allow enemies to walk across, but remain targets and block other births. */
    std::vector<Obstacle> enemyObstacles() const;

    /** The bodies are items (SetItemGeo sorts every item node): an alpha-textured one
     * such as the vermin's skull pile waits for the blended pass, after the solids. */
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 presentationAlpha = -1.0f, TreeModel::Pass pass = TreeModel::Pass::All);

    usize count() const { return m_sim.generators.size(); }
    /** Marks `id` out with `highlight` as it is drawn, as the developer console's
     * selection; null, or nobody's id, marks nothing. */
    void setHighlight(s32 id, const SelectionHighlight* highlight) {
        m_highlightedId = id;
        m_highlight = highlight;
    }
    /** Native lifetime identity; zero means no retained slot at this index. */
    u64 instanceOf(s32 id) const {
        return id >= 0 && static_cast<usize>(id) < m_sim.generators.size()
                   ? m_sim.generators[static_cast<usize>(id)].instance
                   : 0;
    }
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
    s32 livingOf(s32 id) const { return m_sim.generators[static_cast<usize>(id)].living; }
    const Vec3& positionOf(s32 id) const;
    /** Actual native attachment, including a breaking generator's final pose. */
    s32 floorOf(s32 id) const {
        return id >= 0 && static_cast<usize>(id) < m_sim.generators.size()
                   ? m_sim.generators[static_cast<usize>(id)].support
                   : -1;
    }
    const Obstacle& boxOf(s32 id) const;
    MissileTarget target(s32 index, s32 id) const;
    /** The generator's parameters as a level's record gives them, for tests and tools. */
    static s32 paramOf(const ItemInstance& instance, usize index);

    /** Pointer-free native body, including dormant, suppressed and destroyed slots. */
    struct Slot {
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
        bool boss = false;
        bool operator==(const Slot& /*other*/) const;
    };
    const Slot& slot(usize index) const { return m_sim.generators[index]; }

private:
    struct Bodies {
        s32 kind = -1;
        const ItemArchive* archive = nullptr;
        std::array<TreeInfo, kStates + 1> trees;
        std::array<TreeModel, kStates + 1> models;
        TextureAnimator textures;
    };
    struct Generator : Slot {
        std::optional<ItemFigure> bossFigure;
        u64 instance = 0;
    };

    Bodies* bodiesOf(s32 kind);
    const Bodies* bodiesOf(s32 kind) const;
    bool loadBodies(RenderDevice& device, Enemies& enemies, s32 kind, ItemArchive* realmItems,
                    std::span<TextureSet* const> lenders);
    s32 stateFor(const Generator& generator, bool destroyed) const;
    void appendImportedHealth(NativeStateDigest& digest) const;
    const SwarmRules* m_swarm = nullptr; ///< borrowed until close; never simulation state
    s32 m_highlightedId = -1;            ///< the developer console's selection
    const SelectionHighlight* m_highlight = nullptr; ///< how it is marked out, while drawn
    bool seen(const Generator& generator) const;
    void updatePresence(Generator& generator, bool seen) const;
    void applyBroodEvents(Enemies& enemies);
    static void bindSupport(Generator& generator, const ItemInfo& info, const Vec3& authored,
                            const WorldCollision* collision);
    /** One generator's standing, authored or asked for. */
    struct Placement {
        const ItemInfo* info = nullptr; ///< the generator's record
        const ItemInfo* body = nullptr; ///< the record its figure's size comes from
        s32 kind = kGruntKind;
        s32 strength = 1;         ///< the tier it breeds at and the state it stands in
        s32 authoredStrength = 1; ///< what its durability and defaults go by
        s32 algorithm = -1;
        s32 most = 0;
        s32 interval = 0;
        s32 minPlayers = 0;
        bool alwaysActive = false;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 rotation{0.0f, 0.0f, 0.0f};
    };
    /** Loads the kind's bodies and stands the generator; false without them or a slot. */
    bool stand(RenderDevice& device, Enemies& enemies, const Placement& placement,
               const WorldCollision* authored, ItemArchive* realmItems,
               std::span<TextureSet* const> lenders);

    struct Simulation {
        const WorldCollision* collision = nullptr;
        std::vector<Generator> generators;
        std::optional<ViewSet> view;
        bool attentionGate = false; ///< a generator counts as seen only near a camera's point
        std::vector<Bodies*> bodies;
        GeneratorScales scales;
        s32 players = 1;
        u32 specialBirth = 0;
        u64 nextInstance = 1;
        bool bound = false;
        std::optional<Mt19937> importedRandom; ///< absent for ordinary GDL scenes
    } m_sim;
    std::vector<std::unique_ptr<Bodies>> m_bodyCache;
    /** Immutable level-presentation ownership, keyed by pool lifetime rather than
     * reusable slot index. Imported levels do not participate in rollback. */
    std::vector<u64> m_externalBodies;
    struct ImportedHealth {
        u64 instance = 0;
        std::array<f32, kStates> remaining{}; ///< cumulative health below each damage state
        std::vector<s32> broodKinds;
        std::vector<s32> broodAlgorithms;
        s32 broodTier = 0;
        struct Breeding {
            std::array<s32, kStates> broodByState{};
            std::array<s32, 2> waitTicks{};
            f32 activeDistance = 50;
        };
        std::optional<Breeding> breeding;
    };
    std::vector<ImportedHealth> m_importedHealth; ///< immutable imported-level durability
    std::vector<ItemInfo> m_records;     ///< the level's generator records, for the console
    ItemArchive* m_realmItems = nullptr; ///< what bind was given, for standings after it
    std::vector<TextureSet*> m_lenders;
    struct BossPrototype {
        const ItemArchive* archive = nullptr;
        RenderDevice* device = nullptr;
        std::string tree;
        ItemFigure figure;
    };
    std::vector<BossPrototype> m_bossPrototypes;
    std::vector<Slot> m_roster;
    u64 m_localEpoch = 0;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);

public:
    class LocalSnapshot {
        friend class Generators;
        Simulation m_state;
        std::vector<TextureAnimator::Clock> m_clocks;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    /** Stage-local native value copies, including boss-born generators. Artwork
     * and collision owners stay loaded; capture/restore never binds resources. */
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    /** Exact same-owner values across every retained slot, including boss art's
     * native clocks/particles. Loaded artwork and draw caches stay outside. */
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    /** Register immutable generated artwork after its source archives are registered. */
    bool registerNativeResources(NativeStateResources& resources) const;
    /** Complete retained native values, with semantic rather than address bindings. */
    bool appendNativeDigest(NativeStateDigest& digest) const;
    /** Foreign/stale history is refused before appending any bytes. */
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    static void appendSimulationDigest(
        NativeStateDigest& digest, const Simulation& state,
        std::optional<std::span<const TextureAnimator::Clock>> savedClocks = std::nullopt);
};

} // namespace gdl::game
