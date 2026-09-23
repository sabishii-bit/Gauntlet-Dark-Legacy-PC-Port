#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/CritterData.h"
#include "game/enemies/CritterProjectile.h"
#include "game/enemies/Enemies.h"

namespace gdl::game {

/** The great creatures the level can hold. */
inline constexpr int kGolemCritter = 3;
inline constexpr int kGargoyleCritter = 7;
inline constexpr int kGeneralCritter = 8;
inline constexpr int kBossCritter = 4; ///< a realm's boss, by its name ("LICH")

/** The boss a level's `bossType` names (34 the dragon to 44 the garm), or nothing. */
std::string_view bossNameOf(int kind);

/** A contact a critter has made. Breath contacts repeat while touching; the
 * recipient's shared breath timer decides when they may damage it again. */
struct CritterBlow {
    int player = -1;
    int critter = -1;
    float damage = 0.0f;
    Vec3 direction{0.0f, 0.0f, 1.0f};
    bool breath = false;
};

/** An effect and a sound a critter has set off: a move's, a strike's or a hit's, where it
 * happened; `follows` for one that rides on the body. */
struct CritterCue {
    int critter = -1;
    std::string tree;  ///< of the critter's own archive; empty for a sound alone
    std::string sound; ///< empty for an effect alone
    Vec3 position{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float scale = 1.0f;
    float life = 0.0f; ///< seconds, when it does not play out
    bool follows = false;
    bool shakes = false;
    std::optional<std::string> node; ///< animated attachment, distinct from a body translation
    Vec3 nodeOffset{0.0f};
};

/** A dying critter's death throwing something out (the coins a boss spews): from where,
 * which way and how fast, and how far round each side of that way. */
struct CritterSpew {
    int critter = -1;
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    float halfAngle = 0.0f; ///< radians
};

/** Experience a critter is worth: a share of its value for each hit, to the hitter, and
 * a fifth of it to everyone (`player` -1) when it falls. */
struct CritterLoss {
    int critter = -1;
    int kind = 0;
    std::string form; ///< a gargoyle's ("EAGL"): the key it drops is named by it
    int player = -1;
    float experience = 0.0f;
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
    static constexpr int kMost = 16;
    static constexpr float kBlockShare = 0.25f;     ///< what gets through a block
    static constexpr float kKillShare = 0.2f;       ///< of the value, to everyone, on a kill
    static constexpr float kRoarAfter = 50.0f;      ///< damage taken before it roars
    static constexpr float kUnderLevelLoss = 0.02f; ///< experience lost a level under the place's
    static constexpr int kTicksPerSecond = 60;

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
    std::optional<int> spawn(int kind, const Vec3& position, float yaw, std::string_view form = "");

    void update(int ticks, float seconds, std::span<const EnemyView> players);
    std::vector<CritterBlow> takeBlows();
    std::vector<CritterLoss> takeLosses();
    /** The effects and sounds set off since the last call. */
    std::vector<CritterCue> takeCues();
    /** What the deaths since the last call threw out. */
    std::vector<CritterSpew> takeSpews();
    std::vector<CritterShot> takeShots();

    void hurt(int id, const EnemyHit& hit);
    /** Stops it where it stands, its animation with it, for `ticks`. */
    void freeze(int id, int ticks);
    /** Takes its targets from it for `ticks`, over which it turns at a tenth of its rate. */
    void blind(int id, int ticks);
    /** Refuses its curbed attacks (those whose harm is flagged so) while `seconds` is over
     * nought; nought lifts the curb. */
    void curb(int id, float seconds);
    void resize(int id, float scale);
    /** Keeps it to its stance between moves while `held`. */
    void hold(int id, bool held);
    /** Has it roar as soon as its move is over. */
    void roar(int id);
    std::vector<MissileTarget> targets() const;
    std::optional<int> struckBy(const Vec3& from, const Vec3& to, float radius) const;
    std::vector<int> within(const Vec3& centre, float radius) const;
    std::vector<int> reachedBy(const Vec3& centre, float radius, float arc,
                               const Vec3& facing) const;

    /** Optional frozen skin is borrowed for this draw only. */
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* frozenTexture = nullptr) const;

    std::size_t count() const;
    bool alive(int id) const;
    bool dying(int id) const;
    int kindOf(int id) const;
    float healthOf(int id) const;
    float maxHealthOf(int id) const;
    /** Floor anchor, not the animation root (which includes the type's floorOffset). */
    const Vec3& positionOf(int id) const;
    float yawOf(int id) const;
    std::optional<Mat4> nodeTransformOf(int id, std::string_view node) const;
    float radiusOf(int id) const;
    int targetOf(int id) const;
    /** The name of the move it is doing ("WALK", "ATTACK1L"). */
    std::string_view moveOf(int id) const;
    /** The type of the move it is doing, -1 with none. */
    int moveTypeOf(int id) const;
    /** Whether the move it is doing has played out. */
    bool moveDoneOf(int id) const;
    bool frozen(int id) const;
    bool blinded(int id) const;
    bool curbed(int id) const;
    float scaleOf(int id) const;
    const CritterData* dataOf(int id) const;
    /** The archive its body and textures came from, or null. */
    ItemArchive* archiveOf(int id);
    /** A gargoyle's form ("EAGL"), empty for the rest. */
    std::string formOf(int id) const;

private:
    enum class State : std::uint8_t { Inactive, Active, Dying };

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
        float health = 0.0f;
        float maxHealth = 1.0f;
        Vec3 position{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        Vec3 push{0.0f, 0.0f, 0.0f};
        int target = -1;
        float targetDistance = 100000.0f;
        int move = -1; ///< the move playing
        bool moveDone = false;
        std::vector<float> cooldowns;    ///< seconds left before each move may be chosen again
        std::vector<int> struckThisMove; ///< players already hurt by the move playing
        float hurtPending = 0.0f;
        unsigned int hurtFlags = 0;
        Vec3 hurtDirection{0.0f, 0.0f, 0.0f};
        float roarOwed = 0.0f; ///< damage taken toward the next roar
        float alpha = 1.0f;
        float scale = 1.0f;
        int frozenTicks = 0;          ///< a legend item's: it stands still this long
        int blindTicks = 0;           ///< and finds no one this long
        float curbSeconds = 0.0f;     ///< over nought, its curbed attacks are refused
        bool held = false;            ///< keeps to its stance between moves
        bool roarWanted = false;      ///< roars as soon as it may
        unsigned int soundsGiven = 0; ///< bits: the move's sound, its second, each strike's
        int shotFrame = -1;
        std::optional<Vec3> attackTarget; ///< captured by a targeted-area move, not a homing point
        AnimationPlayer player;
        TreePose pose;
    };

    Stock* stockFor(int kind, std::string_view form);
    Critter* critterAt(int id);
    static bool startMove(Critter& critter, std::size_t index);
    static void chooseMove(Critter& critter, std::span<const EnemyView> players);
    static std::optional<std::size_t> bestMove(const Critter& critter,
                                               std::span<const EnemyView> players);
    /** Whether a legend item's curb keeps the move from it. */
    static bool curbedMove(const Critter& critter, const CritterMove& move);
    /** What of a sound record is set off. */
    enum class CueParts : std::uint8_t { Both, Sound, Effect };
    /** Sets off sound record `index` (and what it links to) at `position`. */
    void cue(const Critter& critter, int id, int index, const Vec3& position,
             CueParts parts = CueParts::Both, std::optional<std::string_view> node = std::nullopt);

    std::vector<CritterCue> m_cues;
    std::vector<CritterSpew> m_spews;
    std::vector<CritterShot> m_shots;
    void shoot(const Critter& critter, int id, const CritterMove& move, int damageIndex,
               std::span<const EnemyView> players);
    char m_levelLetter = 'G';
    void strikeWith(Critter& critter, int id, const CritterMove& move, int damageIndex,
                    std::span<const EnemyView> players);
    static Vec3 partPosition(const Critter& critter, std::string_view node);
    static Mat4 partTransform(const Critter& critter, std::string_view node);
    static Mat4 modelTransform(const Critter& critter);
    void carry(Critter& critter, float seconds, const CritterMove* move,
               std::span<const EnemyView> players);
    static void chooseTarget(Critter& critter, std::span<const EnemyView> players);
    static const EnemyView* viewOf(std::span<const EnemyView> players, int player);

    RenderDevice* m_device = nullptr;
    std::filesystem::path m_root;
    const WorldCollision* m_collision = nullptr;
    EnemyScales m_scales;
    char m_realm = 'G';
    std::vector<std::unique_ptr<Stock>> m_stocks;
    std::array<Critter, kMost> m_critters;
    std::vector<CritterBlow> m_blows;
    std::vector<CritterLoss> m_losses;
    float m_textureFrames = 0.0f;
};

} // namespace gdl::game
