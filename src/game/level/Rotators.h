#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldLighting.h"
#include "engine/world/WorldScene.h"

#include "game/level/ItemFigure.h"
#include "game/level/LevelTriggers.h"

namespace gdl::game {

/** What a rotator's object did this update, for the level to sound. */
struct RotatorCue {
    enum class Kind : u8 { Turning, Stopped };
    Kind kind = Kind::Turning;
    usize rotator = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    bool operator==(const RotatorCue&) const = default;
    template <class Digest> friend void visitNativeState(Digest& digest, const RotatorCue& value) {
        digest.template fields<3>(value);
    }
};

/**
 * A level's rotators (item type 12): world objects turned about their own upright axis at the
 * record's pace. One of subtype 0 turns for ever; one of subtype 2 is a bridge pad that waits
 * for someone to step on it, then turns its object through the record's angle and stops there
 * for good (the turntables that line bridges up).
 */
class Rotators {
public:
    Rotators() = default;
    Rotators(const Rotators&) = delete;
    Rotators& operator=(const Rotators&) = delete;
    Rotators(Rotators&&) noexcept = default;
    Rotators& operator=(Rotators&&) noexcept = default;
    ~Rotators() = default;

    static constexpr s32 kSpinning = 0;
    static constexpr s32 kTurnedByPad = 2;
    static constexpr f32 kTicksPerSecond = 60.0f; ///< the record's pace is a turn per tick

    struct Rotator {
        s32 instance = -1;
        s32 object = -1;
        s32 subtype = kSpinning;
        f32 speed = 0.0f; ///< radians a tick, the way YawMat3 turns
        f32 limit = 0.0f; ///< how far a pad's object turns, either way
        f32 turned = 0.0f;
        Vec3 origin{0.0f, 0.0f, 0.0f}; ///< the object's place under its parent
        Vec3 spot{0.0f, 0.0f, 0.0f};   ///< the pad, where it is stepped on
        f32 radius = 0.0f;
        f32 height = 0.0f;
        bool started = false;
        bool done = false;
        std::optional<ItemFigure> pad;
        bool operator==(const Rotator&) const = default;
        template <class Digest> friend void visitNativeState(Digest& digest, const Rotator& value) {
            digest.template fields<13>(value);
        }
    };

    void bind(const WorldLayout& layout);
    void bindFigures(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope);
    void clear();
    usize size() const { return m_sim.rotators.size(); }
    const Rotator& rotator(usize index) const { return m_sim.rotators[index]; }
    /** Turns what turns for `seconds`, starting pads that are stepped on. */
    std::vector<RotatorCue> update(f32 seconds, std::span<const TriggerVisitor> visitors,
                                   WorldScene& scene);
    static constexpr usize kMaxCursors = 2048;
    struct Cursor {
        s32 instance = -1;
        s32 object = -1;
        f32 turned = 0;
        bool started = false;
        bool done = false;
        bool valid() const;
        bool operator==(const Cursor&) const = default;
    };
    std::vector<Cursor> checkpoint() const;
    bool accepts(std::span<const Cursor> state) const;
    bool restore(std::span<const Cursor> state);
    /** Advance already-started rotators only; no pad contact or sound cue dispatch. */
    bool advanceMotion(f32 seconds, WorldScene& scene);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    static void place(const Rotator& rotator, WorldScene& scene);
    static std::optional<RotatorCue::Kind> advance(Rotator& rotator, f32 ticks, WorldScene& scene);

    struct Simulation {
        std::vector<Rotator> rotators;
        bool operator==(const Simulation&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const Simulation& value) {
            digest.template fields<1>(value);
        }
    } m_sim;

public:
    /** Native rotator values and pad clocks; world transforms and returned cues are
     * separate owners and must be restored by the enclosing scene. */
    class LocalSnapshot {
        friend class Rotators;
        Simulation m_state;
        std::weak_ptr<const u8> m_binding;
        const Rotators* m_owner = nullptr;
        u64 m_epoch = 0;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    u64 m_localEpoch = 0;
    bool appendDigest(NativeStateDigest& digest, const Simulation& state) const;
    const WorldLayout* m_layout = nullptr;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
    u64 m_checkpointGeneration = 0;
};

} // namespace gdl::game
