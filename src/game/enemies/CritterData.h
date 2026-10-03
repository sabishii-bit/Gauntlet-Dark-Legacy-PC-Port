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

#include "game/enemies/CombatantKind.h"
#include "game/enemies/CritterMovement.h"
#include "game/enemies/MoveDefinition.h" // IWYU pragma: export

namespace gdl::game {

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
    s32 childIndex() const { return m_childIndex; }
    s32 parentIndex() const { return m_parentIndex; }
    std::string_view rootNode() const { return m_rootNode; }
    bool loaded() const { return !m_moves.empty(); }

    std::string_view name() const { return m_name; }         ///< "GOLEM"
    std::string_view folder() const { return m_folder; }     ///< "golem": the archive's
    std::string_view prefix() const { return m_prefix; }     ///< "GOLEM"
    std::string tree() const { return m_prefix + m_suffix; } ///< "GOLEM1"
    CombatantKind kind() const { return m_kind; } ///< descriptor family, not a boss encounter id
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
