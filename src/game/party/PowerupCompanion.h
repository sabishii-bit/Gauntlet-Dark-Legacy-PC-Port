#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCamera.h"

#include "game/characters/Inventory.h"
#include "game/party/CompanionVisual.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

class NativeStateResources;

/**
 * The companion a powerup brings along (PlayerProcessPowerups, player.c 5789): Pojo, the fire
 * shield's blaze while its bearer runs with it, the phoenix, a breath on the head, or wings on
 * the back, one at a time in that order, apart from any familiar the character has earned.
 * Pojo acts out what the body does; the others idle, and the phoenix spits as its bearer
 * attacks. It fades over the last second of the powerups that bring companions.
 */
class PowerupCompanion {
public:
    enum class Kind : u8 {
        None,
        Pojo,
        FireShield,
        Phoenix,
        FireBreath,
        AcidBreath,
        ElecBreath,
        Wings
    };
    /** What it hangs from: the body (p->node), the head (weapon_node), or the back (the
     * root's first child's first child). */
    enum class Mount : u8 { Body, Head, Back };

private:
    struct Resource {
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        TreeModel model;
        TextureAnimator textures; ///< resolved bank; its sampled clock is derived from State
    };
    struct Resources {
        std::vector<std::unique_ptr<Resource>> entries;
    };
    /** Ordinary values only. Resource addresses remain stable across kind changes. */
    struct State {
        Kind kind = Kind::None;
        Resource* resource = nullptr;
        const TreeInfo* poseTree = nullptr; ///< clear preserves the native dormant pose
        TreePose pose;
        AnimationPlayer player;
        TextureAnimator::Clock textures;
        // The saved interval must survive a rewind even before the next update.
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
        bool presentationAdvanced = false;
    };

public:
    /** In-process rewind, including Pojo's projectile attachment pose. Never uploads
     * or reconstructs models. All borrowed archives must outlive this owner. */
    class LocalSnapshot {
        friend class PowerupCompanion;
        State m_state;
        std::weak_ptr<const Resources> m_owner;
    };
    void saveLocal(LocalSnapshot& state) const;
    bool canRestoreLocal(const LocalSnapshot& state) const;
    bool restoreLocal(const LocalSnapshot& state);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    /** Archives are registered by their owners before retained companion banks. */
    bool registerNativeResources(NativeStateResources& registry) const;
    /** Pojo's sequences, by index (the POJO tree's READY, RUN, ATTACK, ATTPWR, HIT, DEATH). */
    static constexpr s32 kReady = 0;
    static constexpr s32 kRun = 1;
    static constexpr s32 kAttack = 2;
    static constexpr s32 kPower = 3;
    static constexpr s32 kHit = 4;
    static constexpr s32 kDeath = 5;

    /** The companion `worn` brings, while the body `shieldRunning` (SHIELD_RUN). */
    static Kind choose(const PowerupEffects& worn, bool shieldRunning);
    static std::string_view treeOf(Kind kind);
    static Mount mountOf(Kind kind);
    /** Shared material setup for local and replicated companion models. */
    static void configureModel(TreeModel& model, const TreeInfo& tree, ModelSet& models);
    /** Whether the kind's tree is the weapon archive's (FW_SHLD_ACTIVE); else the powerups'. */
    static bool fromWeapons(Kind kind) { return kind == Kind::FireShield; }
    /** Pojo's sequence for what the body is doing (player.c 5812): the run for moving, the
     * hit for any reaction or fall, the death for dying, the power one for a breath. */
    static s32 pojoSequenceOf(PlayerAnimator::Action action);
    /** How much of the companion shows: all of it until the last second of the powerups that
     * bring companions (familiar_time), fading through that second. */
    static f32 fadeOf(const Inventory& inventory);

    /** Brings the companion `kind` up from its archive, when it changed. */
    void choose(RenderDevice& device, Kind kind, ItemArchive& powerups, ItemArchive* weapons);
    /** Plays on by `seconds`: Pojo as the body asks (`swung`: a swing landed), the others
     * idling, the phoenix's attack as `spat`. */
    void update(f32 seconds, PlayerAnimator::Action action, bool swung, bool spat);
    void capturePresentation() {
        m_state.presentationAdvanced = false;
        m_state.textures.advance = 0;
    }
    void draw(RenderDevice& device, const Mat4& clip, const Mat4& at, const WorldLighting& lighting,
              f32 alpha, const CameraFrame* camera, f32 renderAlpha = -1.0f,
              TreeModel::Pass pass = TreeModel::Pass::All) const;
    void clear();

    Kind kind() const { return m_state.kind; }
    bool shown() const { return m_state.resource != nullptr; }
    std::optional<CompanionVisual> visual(const Mat4& at, f32 alpha) const;
    std::optional<Mat4> attachment(const Mat4& at, std::string_view object) const;
    s32 sequence() const { return shown() ? static_cast<s32>(m_state.player.sequence()) : -1; }
    AnimationPlayer::Checkpoint animationCheckpoint() const { return m_state.player.checkpoint(); }

private:
    bool appendState(NativeStateDigest& digest, const State& state) const;
    void play(s32 sequence);
    /** Whether `sequence` loops: the stance, and Pojo's run; the rest play once. */
    bool loops(s32 sequence) const {
        return sequence == kReady || (m_state.kind == Kind::Pojo && sequence == kRun);
    }

    const TreeInfo* tree() const { return shown() ? m_state.resource->tree : nullptr; }
    Resource* findResource(ItemArchive& archive, const TreeInfo& tree) const;
    State m_state;
    // Copies share only retained artwork, never animation state. Local rewind
    // itself copies State, not this cache.
    std::shared_ptr<Resources> m_resources = std::make_shared<Resources>();
};

} // namespace gdl::game
