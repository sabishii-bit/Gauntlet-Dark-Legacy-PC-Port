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
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/CritterArea.h"
#include "game/enemies/CritterData.h"
#include "game/enemies/CritterProjectile.h"
#include "game/enemies/Enemies.h"

namespace gdl::game {

/** The great creatures the level can hold. */
inline constexpr s32 kGolemCritter = 3;
inline constexpr s32 kGargoyleCritter = 7;
inline constexpr s32 kGeneralCritter = 8;
inline constexpr s32 kBossCritter = 4; ///< a realm's boss, by its name ("LICH")

/** The boss a level's `bossType` names (34 the dragon to 44 the garm), or nothing. */
std::string_view bossNameOf(s32 kind);

/** A contact a critter has made. Breath contacts repeat while touching; the
 * recipient's shared breath timer decides when they may damage it again. */
struct CritterBlow {
    s32 player = -1;
    s32 critter = -1;
    f32 damage = 0.0f;
    Vec3 direction{0.0f, 0.0f, 1.0f};
    bool breath = false;
    u32 flags = 0;     ///< authored player damage modifiers, not the attack's behavior flags
    Vec3 origin{0.0f}; ///< emitted segment origin, used for breath cover queries
    bool area = false;
    f32 repeatGap = 0.0f; ///< area-effect immunity requested on contact
};

/** An effect and a sound a critter has set off: a move's, a strike's or a hit's, where it
 * happened; `follows` for one that rides on the body. */
struct CritterCue {
    s32 critter = -1;
    std::string tree;  ///< of the critter's own archive; empty for a sound alone
    std::string sound; ///< empty for an effect alone
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
    f32 life = 0.0f; ///< seconds, when it does not play out
    bool follows = false;
    bool shakes = false;
    bool rootAttachment = false;     ///< root transform, not a fixed world-space body offset
    std::optional<std::string> node; ///< animated attachment, distinct from a body translation
    Vec3 nodeOffset{0.0f};
    Vec2 pitchYaw{0.0f}; ///< local effect rotation, independent of its attachment offset
    bool loop = true;
};

/** A dying critter's death throwing something out (the coins a boss spews): from where,
 * which way and how fast, and how far round each side of that way. */
struct CritterSpew {
    s32 critter = -1;
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    f32 halfAngle = 0.0f; ///< radians
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
    static constexpr f32 kBlockShare = 0.25f;     ///< what gets through a block
    static constexpr f32 kKillShare = 0.2f;       ///< of the value, to everyone, on a kill
    static constexpr f32 kRoarAfter = 50.0f;      ///< damage taken before it roars
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
    /** The effects and sounds set off since the last call. */
    std::vector<CritterCue> takeCues();
    /** What the deaths since the last call threw out. */
    std::vector<CritterSpew> takeSpews();
    std::vector<CritterShot> takeShots();

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

    /** Optional frozen skin is borrowed for this draw only. */
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* frozenTexture = nullptr) const;

    usize count() const;
    bool alive(s32 id) const;
    bool dying(s32 id) const;
    s32 kindOf(s32 id) const;
    f32 healthOf(s32 id) const;
    f32 maxHealthOf(s32 id) const;
    /** Floor anchor, not the animation root (which includes the type's floorOffset). */
    const Vec3& positionOf(s32 id) const;
    f32 yawOf(s32 id) const;
    std::optional<Mat4> nodeTransformOf(s32 id, std::string_view node) const;
    std::optional<Mat4> rootTransformOf(s32 id) const;
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
        TextureAnimator textures;
    };

    struct Critter {
        State state = State::Inactive;
        Stock* stock = nullptr;
        f32 health = 0.0f;
        f32 maxHealth = 1.0f;
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 yaw = 0.0f;
        f32 initialYaw = 0.0f;
        Vec3 homePosition{0.0f}; ///< floor-space home anchor, independent of current position
        Vec3 initialRoot{0.0f};  ///< geometry's saved base, distinct from an explicit roaming home
        Vec3 push{0.0f, 0.0f, 0.0f};
        s32 target = -1;
        f32 targetDistance = 100000.0f;
        s32 move = -1; ///< the move playing
        bool moveDone = false;
        f32 finishedSeconds = 0.0f;
        f32 age = 0.0f;
        std::vector<f32> moveTimes;
        std::vector<f32> patternTimes;
        s32 pattern = -1;
        usize patternStep = 0;
        std::vector<f32> cooldowns;      ///< seconds left before each move may be chosen again
        std::vector<s32> struckThisMove; ///< players already hurt by the move playing
        std::vector<CritterArea> areas;
        f32 hurtPending = 0.0f;
        u32 hurtFlags = 0;
        Vec3 hurtDirection{0.0f, 0.0f, 0.0f};
        f32 roarOwed = 0.0f; ///< damage taken toward the next roar
        f32 alpha = 1.0f;
        f32 scale = 1.0f;
        s32 frozenTicks = 0;     ///< a legend item's: it stands still this long
        s32 blindTicks = 0;      ///< and finds no one this long
        f32 curbSeconds = 0.0f;  ///< over nought, its curbed attacks are refused
        bool held = false;       ///< keeps to its stance between moves
        bool roarWanted = false; ///< roars as soon as it may
        u32 soundsGiven = 0;     ///< bits: the move's sound, its second, each strike's
        s32 shotFrame = -1;
        std::optional<Vec3> attackTarget; ///< captured by a targeted-area move, not a homing point
        AnimationPlayer player;
        TreePose pose;
    };

    Stock* stockFor(s32 kind, std::string_view form);
    Critter* critterAt(s32 id);
    static bool startMove(Critter& critter, usize index, bool recordUse = true);
    static void chooseMove(Critter& critter, std::span<const EnemyView> players);
    static std::optional<usize> bestMove(const Critter& critter,
                                         std::span<const EnemyView> players);
    static bool chooseBossAttack(Critter& critter, std::span<const EnemyView> players);
    static s32 attackTarget(const Critter& critter, const CritterTarget& criteria,
                            std::span<const EnemyView> players);
    static f32 attackRate(const Critter& critter);
    void startArea(Critter& critter, s32 id, const CritterDamage& damage);
    void updateAreas(Critter& critter, s32 id, std::span<const EnemyView> players);
    /** Whether a legend item's curb keeps the move from it. */
    static bool curbedMove(const Critter& critter, const CritterMove& move);
    /** Sets off sound record `index` (and what it links to) at `position`. */
    void cue(const Critter& critter, s32 id, s32 index, const Vec3& position,
             std::optional<std::string_view> node = std::nullopt);

    std::vector<CritterCue> m_cues;
    std::vector<CritterSpew> m_spews;
    std::vector<CritterShot> m_shots;
    void shoot(const Critter& critter, s32 id, const CritterMove& move, s32 damageIndex,
               std::span<const EnemyView> players);
    char m_levelLetter = 'G';
    void strikeWith(Critter& critter, s32 id, const CritterMove& move, s32 damageIndex,
                    std::span<const EnemyView> players);
    static Vec3 partPosition(const Critter& critter, std::string_view node);
    static Mat4 partTransform(const Critter& critter, std::string_view node);
    static Mat4 attachmentTransform(const Critter& critter, std::string_view node);
    static Mat4 modelTransform(const Critter& critter);
    void carry(Critter& critter, f32 seconds, const CritterMove* move,
               std::span<const EnemyView> players);
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
    f32 m_textureFrames = 0.0f;
};

} // namespace gdl::game
