#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/critters/CombatEvents.h"
#include "game/critters/CombatantKind.h"
#include "game/critters/CritterMovement.h"
#include "game/critters/MoveDefinition.h" // IWYU pragma: export

namespace gdl::formats {
struct CritterFile;
}

namespace gdl::game {

/** Immutable combat rules supplied by an asset adapter. Defaults retain GDL's
 * authored behavior; imported records need not pretend their runtime is identical. */
struct CritterRules {
    f32 projectileMaxRate = 1.5f;
    f32 projectileRateScale = 0.75f;
    f32 ballisticTargetOffset = 0;
    bool linearRadialAreas = false;
    f32 areaHalfHeight = 0;
    f32 pressureReferenceDamage = 0;
    bool nativeImpactAreas = false;
    bool worldAxisBreathPitch = false;
    /** Negative retains the class cylinder midpoint; otherwise measured from feet. */
    f32 projectileTargetHeight = -1;
    bool restoreCaptureSupport = false;
    /** A part's offset turns with its node. GDL's GetWorldMat (0x800BB614) instead adds it in
     * the frame of the node's parent, so the node's own turn and scale never move it. */
    bool nodeFramePartOffsets = false;
    /** A blow's push, along the striking node's swing and away from the body's origin, keeps
     * only its flat way, at a unit's length. GDL's CritterNodePlayerCollide (0x80036424) knocks a
     * player by the whole push, longest when the swing runs outward. */
    bool unitBlowPush = false;
    /** A breath pushes a player flat away from where the breath starts, at a unit's length, or
     * along its beam when the player stands next to no flat distance from that start. GDL's
     * CritterFirePlayerCollide (0x80036138) pushes along the beam alone. */
    bool breathPushFromOrigin = false;
    /** Seconds by which its blow or breath sets a struck player's fxhittime ahead: the wait the
     * great ones' breaths and gated blows keep before striking that player again. GDL's
     * CritterDamagePlayer (0x800367CC) sets a quarter second. */
    f32 playerBreathGap = kPlayerBreathGap;
    /** Rules an adapter selects where its fighter is not Dark Legacy's, a bit each: the port's
     * own where the fighter's binary is unread, or that binary's where it is read. */
    u32 portRules = 0;
    /** portRules: its nodes' scale keys carry and stretch where it strikes, is struck and holds
     * a player, as they do its draw. GDL's GetWorldMat (0x800BB614) composes the nodes without
     * them, which only the draw applies. */
    static constexpr u32 kScaledNodes = 0x1;
    /** portRules: its blows, breaths, shots, grabs and spews measure from the move's node after
     * its look has turned the look nodes. GDL's CritterMoveSetup leaves worldMoveMatrix
     * (0x8003B1CC) for CritterActivate's strikes before CritterLookAtPlayer turns them
     * (CritterGolemAI 0x80039A08-0x80039A44, CritterBossAI 0x8003A1FC-0x8003A2A8). */
    static constexpr u32 kStrikesAfterGaze = 0x2;
    /** portRules: its blows strike in every update of their windows, whatever the move. GDL's
     * CritterCopyAnim (0x8003C11C) does so only for a claw, a breath, a charge and a grab's
     * first slot, and tests every other blow once a move, on its first frame at or past the
     * blow's start. */
    static constexpr u32 kBlowsEveryFrame = 0x4;
    /** portRules: a body's move naming no node strikes from the model, the node its tree hangs
     * from; a head's still strikes from its own root node. GDL's CritterNewInst makes the
     * tree's root node the body's c->anim (critter.c 2010-2012), which CritterMoveSetup falls
     * back to (0x8003B1CC). */
    static constexpr u32 kModelFallback = 0x20;
};

struct CombatantAttachment {
    std::string tree;
    std::string node;
    Vec3 offset{0};
    bool follows = false; ///< ADDA bit 0, otherwise rooted in the world
};

/** What the original keeps of a great creature in its `CRITTER/<NAME>.WAD`. */
class CritterData {
public:
    /** Loads the native WAD first, accepting a legacy JSON path or fixture as well. */
    bool load(const std::filesystem::path& file, usize typeIndex = 0);
    /** Builds the common combat description from a validated platform decoder. */
    static CritterData fromDecoded(std::string_view name, const formats::CritterFile& source,
                                   usize typeIndex = 0, CritterRules rules = {});
    const CritterRules& rules() const { return m_rules; }
    s32 childIndex() const { return m_childIndex; }
    s32 parentIndex() const { return m_parentIndex; }
    std::string_view rootNode() const { return m_rootNode; }
    bool loaded() const { return !m_moves.empty(); }

    std::string_view name() const { return m_name; }         ///< "GOLEM"
    std::string_view folder() const { return m_folder; }     ///< "golem": the archive's
    std::string_view prefix() const { return m_prefix; }     ///< "GOLEM"
    std::string tree() const { return m_prefix + m_suffix; } ///< "GOLEM1"
    CombatantKind kind() const { return m_kind; } ///< descriptor family, not a boss encounter id
    /** The type flags: what the body is like. */
    static constexpr u32 kShadowed = 0x1; ///< a shadow under it
    static constexpr u32 kByParts = 0x2;  ///< aimed at and walked into part by part
    static constexpr u32 kSquareBounds = 0x20;
    static constexpr u32 kInitialStepBasis = 0x40;
    /** Asleep, it arms its wake only once its floor's trigger moves it (CritterBossAI). */
    static constexpr u32 kSupportActivated = 0x80;
    static constexpr u32 kSolidParts = 0x100; ///< its solid parts stop a body
    static constexpr u32 kUnrestrictedTurn = 0x400;
    u32 typeFlags() const { return m_typeFlags; }
    f32 radius() const { return m_radius; }
    f32 wallRadius() const { return m_wallRadius; }
    const CritterMovement& movement() const { return m_movement; }
    f32 armor() const { return m_armor; }
    /** What it deals an item it walks into, at the level's enemy damage (TYPE +0xB8). */
    f32 itemDamage() const { return m_itemDamage; }
    u32 shieldFlags() const { return m_shieldFlags; }
    f32 maxHealth() const { return m_maxHealth; }
    f32 experience() const { return m_experience; }
    f32 wakeThreshold() const { return m_wake; } ///< how near the party comes before a boss stirs
    f32 vertDrift() const { return m_vertDrift; }
    /** Whether it lies a shadow under it (TYPE flag 1, CritterInitGeo). */
    bool shadowed() const { return m_shadowed; }
    /** Height of the model root above the floor anchor; may be negative. */
    f32 floorOffset() const { return m_floorOffset; }
    const Vec3& originOffset() const { return m_originOffset; }
    /** Where a fallen boss's key appears from the root it was first stood at, in world axes. */
    const Vec3& keyOffset() const { return m_keyOffset; }
    const TargetCriteria& sight() const { return m_sight; }
    const HealthMeterDefinition& meter() const { return m_meter; }
    std::span<const MoveDefinition> moves() const { return m_moves; }
    std::span<const AttackPattern> patterns() const { return m_patterns; }
    std::span<const AttackDefinition> damages() const { return m_damages; }
    std::span<const CritterPart> parts() const { return m_parts; }
    /** The nodes turned to its target: the head, then the eyes; either may name none. */
    std::span<const LookDefinition> looks() const { return m_looks; }
    std::span<const CombatEffectDefinition> sounds() const { return m_sounds; }
    std::span<const CombatantAttachment> attachments() const { return m_attachments; }
    const AttackDefinition* damage(s32 index) const;
    const CombatEffectDefinition* sound(s32 index) const;
    /** The sound records started where it is struck: by a missile, by a blow. */
    s32 hitSoundFar() const { return m_hitSoundFar; }
    s32 hitSoundClose() const { return m_hitSoundClose; }
    /** The first move of a type, if any. */
    std::optional<usize> moveOfType(s32 type) const;
    std::optional<usize> moveNamed(std::string_view name) const;

private:
    CritterRules m_rules;
    void assignDecoded(std::string_view name, const formats::CritterFile& source, usize typeIndex);
    bool loadNative(const std::filesystem::path& file, usize typeIndex);

    s32 m_childIndex = -1;
    s32 m_parentIndex = -1;
    std::string m_rootNode;
    std::string m_name;
    std::string m_folder;
    std::string m_prefix;
    std::string m_suffix;
    CombatantKind m_kind = CombatantKind::Unknown;
    u32 m_typeFlags = 0;
    f32 m_radius = 1.0f;
    f32 m_wallRadius = 1.0f;
    CritterMovement m_movement;
    f32 m_armor = 0.0f;
    f32 m_itemDamage = 0.0f;
    u32 m_shieldFlags = 0;
    f32 m_maxHealth = 1.0f;
    f32 m_experience = 0.0f;
    f32 m_wake = 0.0f;
    f32 m_vertDrift = 0.0f;
    bool m_shadowed = false;
    f32 m_floorOffset = 0.0f;
    Vec3 m_originOffset{0.0f, 0.0f, 0.0f};
    Vec3 m_keyOffset{0.0f, 0.0f, 0.0f};
    TargetCriteria m_sight;
    HealthMeterDefinition m_meter;
    std::vector<MoveDefinition> m_moves;
    std::vector<AttackPattern> m_patterns;
    std::vector<AttackDefinition> m_damages;
    std::vector<CritterPart> m_parts;
    std::array<LookDefinition, 2> m_looks;
    std::vector<CombatEffectDefinition> m_sounds;
    std::vector<CombatantAttachment> m_attachments;
    s32 m_hitSoundFar = -1;
    s32 m_hitSoundClose = -1;
};

} // namespace gdl::game
