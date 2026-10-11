#pragma once

#include <utility>

#include "engine/world/SceneGeometry.h"

#include "game/netplay/FighterSnapshot.h"
#include "game/netplay/HudSnapshot.h"
#include "game/netplay/MotionSnapshot.h"

namespace gdl::game {

/** Authored sequence cursor, never a request to run animation events. Sequence
 * and action indexes require the session's matching build/assets handshake.
 * Generation changes on restart or wrap, so readers cannot blend two swings.
 * Zero generation denotes an unavailable figure/animation. */
struct CombatAnimation {
    u32 action = 0;
    u32 sequence = 0;
    u64 generation = 0;
    f32 frame = 0;
    f32 transition = 1;
    bool valid() const;
};

enum class ReplicaPlayerLife : u8 { Standing, Dying, InTower };
enum class ReplicaEnemyLife : u8 { Active, Asleep, Dying };

/** A per-seat companion mesh: slot 0 is familiar tier 1/2; slot 1 is the
 * powerup companion kind 1..7. Assets are selected before admitting packets. */
struct CompanionState {
    u32 form = 0;
    Mat4 placement{1};
    CombatAnimation animation;
    f32 textureClock = 0;
    f32 alpha = 1;
    bool valid(usize slot) const;
};

/** Host floor contact for the costume's shadow, not guest-side physics. */
struct PlayerShadowState {
    Vec3 ground{0};
    Vec3 normal{0, 1, 0};
    f32 alpha = 1;
    bool valid() const;
};

struct PlayerCombatState {
    f32 health = 0;
    ReplicaPlayerLife life = ReplicaPlayerLife::Standing;
    bool hitFlash = false;
    bool damageable = false;
    CombatAnimation animation;
    std::array<std::optional<CompanionState>, 2> companions{};
    /** Present only for departing survivors: 0 starts the effect, 1 hides the body.
     * Separate from motion: the collision anchor does not sink with the artwork. */
    std::optional<f32> portalPhase = std::nullopt;
    std::optional<PlayerShadowState> shadow = std::nullopt;
};

struct EnemyCombatState {
    u64 instance = 0; // not an enemy-pool slot
    u32 kind = 0;
    u32 tier = 1;
    u32 variant = 0;
    ReplicaEnemyLife life = ReplicaEnemyLife::Active;
    bool hitFlash = false;
    f32 health = 0;
    f32 fullHealth = 0;
    Vec3 position{0};
    f32 yaw = 0;
    CombatAnimation animation;
};

/** IDs live in a source namespace: separate pools may use the same counter.
 * Resource IDs are negotiated by the trusted asset roster, never file paths or
 * process pointers. These are visuals only, with no damage or collision inputs. */
enum class ProjectileSource : u8 { Player, Enemy, PlayerEffect, WorldEffect, Streak, Arrival };
struct ProjectileState {
    static constexpr u32 kAlong = 1;
    static constexpr u32 kSolidWorld = 2;
    static constexpr u32 kUnlit = 4;
    static constexpr u32 kDepthWrite = 8;
    static constexpr u32 kAdditive = 16;
    ProjectileSource source = ProjectileSource::Player;
    u64 instance = 0;
    u32 continuity = 1;
    u32 resource = 0;
    Mat4 placement{1};
    Vec3 direction{0};
    f32 age = 0;
    f32 radius = 1;
    f32 forward = 0;
    Color tint = Color::white();
    CombatAnimation animation;
    f32 textureFrame = 0;
    f32 alpha = 1;
    u32 flags = kDepthWrite;
    auto key() const { return std::pair{source, instance}; }
    bool valid() const;
};

/** Visible pickup meshes only. IDs are append-only item indexes within an epoch;
 * absence removes an item. Collection, amounts, physics and ownership stay on the
 * host. Resource IDs refer to the trusted load-time archive/tree roster. */
struct PickupState {
    u64 instance = 0;
    u32 resource = 0;
    u32 continuity = 1;
    Mat4 placement{1};
    CombatAnimation animation;
    f32 textureFrame = 0;
    f32 alpha = 1;
    bool valid() const;
};

enum class FixtureSource : u8 {
    Chest,
    ChestPreview,
    Gate,
    Switch,
    Generator,
    Barrel,
    Trap,
    Rock,
    Rubble,
    Statue,
    Portal,
    SecretPortal
};
struct FixtureState {
    FixtureSource source = FixtureSource::Chest;
    u64 instance = 0;
    u32 resource = 0;
    u32 continuity = 1;
    Mat4 placement{1};
    CombatAnimation pose;
    u32 meshSequence = 0;
    f32 meshFrame = 0;
    u32 textureSequence = 0;
    f32 textureFrame = 0;
    f32 textureClock = 0;
    f32 alpha = 1;
    bool cameraFacing = false;
    auto key() const { return std::pair{source, instance}; }
    bool valid() const;
};

/** Atomic host checkpoint, including its motion/camera tick. Absence removes an
 * entity; a changed instance is a spawn, never a resurrection of a recycled slot.
 * This slice covers player health/actions, ordinary enemies and fighter meshes.
 * Inventory and one-shot audio/particle VFX are separate work. Pickup and
 * projectile models, effect-tree meshes, streaks and the level's moving/fading scene geometry share
 * this same atomic checkpoint. Geometry is optional for transport probes; a playable replica view
 * requires it. */
struct CombatSnapshot {
    static constexpr usize kMaxEnemies = 128;
    static constexpr usize kMaxProjectiles = 128;
    static constexpr usize kMaxPickups = 384;
    static constexpr usize kMaxFixtures = 1024;
    MotionSnapshot motion;
    std::array<std::optional<PlayerCombatState>, InputCommand::kSeats> players;
    std::vector<EnemyCombatState> enemies;    // strictly increasing instance IDs
    std::vector<ProjectileState> projectiles; // strictly increasing source/instance pairs
    std::vector<PickupState> pickups;         // strictly increasing instance IDs
    std::vector<FixtureState> fixtures;       // strictly increasing source/instance pairs
    std::vector<FighterMeshState> fighters;   // strictly increasing actor/part pairs
    std::optional<SceneGeometry> geometry;
    std::optional<HudSnapshot> hud;
    bool valid() const;
};

/** Full snapshot payload, not a datagram: use CombatReplica::packets for bounded
 * chunks. Explicit scalar encoding; never memcpy game objects or save records. */
class CombatPacket {
public:
    static constexpr usize kHeaderBytes = 36;
    static constexpr usize kCompanionBytes = 84;
    static constexpr usize kShadowBytes = 28;
    static constexpr usize kPlayerBytes = 36 + 2 * kCompanionBytes + kShadowBytes;
    static constexpr usize kEnemyBytes = 72;
    static constexpr usize kProjectileBytes = 148;
    static constexpr usize kPickupBytes = 96;
    static constexpr usize kFixtureBytes = 120;
    static constexpr usize kGeometryHeaderBytes = 20;
    static constexpr usize kGeometryObjectBytes = 64;
    static constexpr usize kMaxBytes =
        kHeaderBytes + MotionPacket::kMaxBytes + InputCommand::kSeats * kPlayerBytes +
        CombatSnapshot::kMaxEnemies * kEnemyBytes +
        CombatSnapshot::kMaxProjectiles * kProjectileBytes +
        CombatSnapshot::kMaxPickups * kPickupBytes + CombatSnapshot::kMaxFixtures * kFixtureBytes +
        kGeometryHeaderBytes + SceneGeometry::kMaxStates * kGeometryObjectBytes +
        FighterPacket::kMaxBytes + HudPacket::kMaxBytes;
    static std::optional<std::vector<u8>> encode(const CombatSnapshot& snapshot);
    static std::optional<CombatSnapshot> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
