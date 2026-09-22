#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"
#include "game/enemies/CritterData.h"
#include "game/enemies/Enemies.h"

namespace gdl::game {

/** The great creatures the level can hold. */
inline constexpr s32 kGolemCritter = 3;
inline constexpr s32 kGargoyleCritter = 7;
inline constexpr s32 kGeneralCritter = 8;
inline constexpr s32 kBossCritter = 4; ///< a realm's boss, by its name ("LICH")

/** The boss a level's `bossType` names (34 the dragon to 44 the garm), or nothing. */
std::string_view bossNameOf(s32 kind);

/** A blow a critter has landed. */
struct CritterBlow {
    s32 player = -1;
    s32 critter = -1;
    f32 damage = 0.0f;
    Vec3 direction{0.0f, 0.0f, 1.0f};
};

/** Experience a critter is worth: a share of its value for each hit, to the hitter, and
 * a fifth of it to everyone (`player` -1) when it falls. */
struct CritterLoss {
    s32 critter = -1;
    s32 kind = 0;
    std::string form; ///< a gargoyle's ("EAGL"): the key it drops is named by it
    s32 player = -1;
    f32 experience = 0.0f;
    bool killed = false;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/**
 * The great creatures: golems, generals and gargoyles, each driven by the table of moves
 * the original keeps for it. Every tick the one it is doing plays on; when it is done, or
 * something louder comes, the next is the loudest move whose target rule and cooldown allow
 * it: an attack on a player within its reach and squarely ahead, else a walk toward one it
 * sees, else a turn to face it, else its stance. A move carries the body at its own pace and
 * turns it at its own rate, and over its harmful frames the part it strikes with hurts
 * whoever it touches.
 */
class Critters {
public:
    static constexpr s32 kMost = 16;
    static constexpr f32 kBlockShare = 0.25f;  ///< what gets through a block
    static constexpr f32 kKillShare = 0.2f;    ///< of the value, to everyone, on a kill
    static constexpr f32 kRoarAfter = 50.0f;   ///< damage taken before it roars
    static constexpr f32 kUnderLevelLoss = 0.02f; ///< experience lost a level under the place's
    static constexpr s32 kTicksPerSecond = 60;

    Critters() = default;
    Critters(const Critters&) = delete;
    Critters& operator=(const Critters&) = delete;
    Critters(Critters&&) = delete;
    Critters& operator=(Critters&&) = delete;
    ~Critters();

    /** Prepares for the level: its realm letter picks the golem's and general's costume. */
    void open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const WorldCollision* collision, const EnemyScales& scales, char realm);
    void close();

    /** Stands one of `kind` (a golem, a general, a gargoyle by its form: "GAR_EAGL", or a
     * boss by its name: "LICH") at `position` facing `yaw`. Nullopt when its data or
     * archive is missing or there is no room. */
    std::optional<s32> spawn(s32 kind, const Vec3& position, f32 yaw, std::string_view form = "");

    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players);
    std::vector<CritterBlow> takeBlows();
    std::vector<CritterLoss> takeLosses();

    void hurt(s32 id, const EnemyHit& hit);
    /** Stops it where it stands, its animation with it, for `ticks`. */
    void freeze(s32 id, s32 ticks);
    /** Takes its targets from it for `ticks`, over which it turns at a tenth of its rate. */
    void blind(s32 id, s32 ticks);
    /** Refuses its curbed attacks (those whose harm is flagged so) while `seconds` is over
     * nought; nought lifts the curb. */
    void curb(s32 id, f32 seconds);
    void resize(s32 id, f32 scale);
    /** Keeps it to its stance between moves while `held`. */
    void hold(s32 id, bool held);
    /** Has it roar as soon as its move is over. */
    void roar(s32 id);
    std::vector<MissileTarget> targets() const;
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    std::vector<s32> within(const Vec3& centre, f32 radius) const;
    std::vector<s32> reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;

    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

    usize count() const;
    bool alive(s32 id) const;
    bool dying(s32 id) const;
    s32 kindOf(s32 id) const;
    f32 healthOf(s32 id) const;
    f32 maxHealthOf(s32 id) const;
    const Vec3& positionOf(s32 id) const;
    f32 yawOf(s32 id) const;
    f32 radiusOf(s32 id) const;
    s32 targetOf(s32 id) const;
    /** The name of the move it is doing ("WALK", "ATTACK1L"). */
    std::string_view moveOf(s32 id) const;
    /** The type of the move it is doing, -1 with none. */
    s32 moveTypeOf(s32 id) const;
    /** Whether the move it is doing has played out. */
    bool moveDoneOf(s32 id) const;
    bool frozen(s32 id) const;
    bool blinded(s32 id) const;
    bool curbed(s32 id) const;
    f32 scaleOf(s32 id) const;
    const CritterData* dataOf(s32 id) const;
    /** The archive its body and textures came from, or null. */
    ItemArchive* archiveOf(s32 id);
    /** A gargoyle's form ("EAGL"), empty for the rest. */
    std::string formOf(s32 id) const;

private:
    enum class State : u8 { Inactive, Active, Dying };

    struct Stock {
        std::string name; ///< "GOLEM", "GAR_EAGL"
        CritterData data;
        ItemArchive archive;
        const TreeInfo* tree = nullptr;
        TreeModel body;
    };

    struct Critter {
        State state = State::Inactive;
        Stock* stock = nullptr;
        f32 health = 0.0f;
        f32 maxHealth = 1.0f;
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 yaw = 0.0f;
        Vec3 push{0.0f, 0.0f, 0.0f};
        s32 target = -1;
        f32 targetDistance = 100000.0f;
        s32 move = -1;                 ///< the move playing
        bool moveDone = false;
        std::vector<f32> cooldowns;    ///< seconds left before each move may be chosen again
        std::vector<s32> struckThisMove; ///< players already hurt by the move playing
        f32 hurtPending = 0.0f;
        u32 hurtFlags = 0;
        Vec3 hurtDirection{0.0f, 0.0f, 0.0f};
        f32 roarOwed = 0.0f;           ///< damage taken toward the next roar
        f32 alpha = 1.0f;
        f32 scale = 1.0f;
        s32 frozenTicks = 0;           ///< a legend item's: it stands still this long
        s32 blindTicks = 0;            ///< and finds no one this long
        f32 curbSeconds = 0.0f;        ///< over nought, its curbed attacks are refused
        bool held = false;             ///< keeps to its stance between moves
        bool roarWanted = false;       ///< roars as soon as it may
        AnimationPlayer player;
        TreePose pose;
    };

    Stock* stockFor(s32 kind, std::string_view form);
    Critter* critterAt(s32 id);
    static bool startMove(Critter& critter, usize index);
    static void chooseMove(Critter& critter, std::span<const EnemyView> players);
    static std::optional<usize> bestMove(const Critter& critter, std::span<const EnemyView> players);
    /** Whether a legend item's curb keeps the move from it. */
    static bool curbedMove(const Critter& critter, const CritterMove& move);
    void strikeWith(Critter& critter, s32 id, const CritterMove& move, s32 damageIndex,
                    std::span<const EnemyView> players);
    static Vec3 partPosition(const Critter& critter, std::string_view node);
    void carry(Critter& critter, f32 seconds, const CritterMove* move, std::span<const EnemyView> players);
    static void chooseTarget(Critter& critter, std::span<const EnemyView> players);
    static const EnemyView* viewOf(std::span<const EnemyView> players, s32 player);

    RenderDevice* m_device = nullptr;
    std::filesystem::path m_root;
    const WorldCollision* m_collision = nullptr;
    EnemyScales m_scales;
    char m_realm = 'G';
    std::vector<std::unique_ptr<Stock>> m_stocks;
    std::array<Critter, kMost> m_critters;
    std::vector<CritterBlow> m_blows;
    std::vector<CritterLoss> m_losses;
};

} // namespace gdl::game
