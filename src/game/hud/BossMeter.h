#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"

#include "game/hud/HealthMeter.h"

namespace gdl::game {

/**
 * A boss's health meter across the top of the screen, as the original lays it out: strips
 * of 256 (two, across the whole virtual width) from the boss's own archive, each a
 * background (`METER_BG1`, `METER_BG2`) under a fill (`METER_FG1`, `METER_FG2`) cropped to
 * the health shown, which eases toward the boss's at three a tick. The first strip's cap
 * and the last strip's tail are never fill, so a full first strip covers half the health.
 * The backgrounds turn blue while the boss stands frozen, and the meter goes with its death.
 */
class BossMeter {
public:
    BossMeter() = default;
    BossMeter(const BossMeter&) = delete;
    BossMeter& operator=(const BossMeter&) = delete;
    BossMeter(BossMeter&&) = delete;
    BossMeter& operator=(BossMeter&&) = delete;
    ~BossMeter() = default;
    struct Look {
        bool visible = false;
        bool frozen = false;
        std::array<s32, 2> widths{};
    };
    static constexpr s32 kY = 8;
    static constexpr s32 kPieceWidth = 256;
    static constexpr s32 kMostPieces = 2;
    static constexpr f32 kEasePerTick = 3.0f;
    static constexpr u8 kAlpha = 143; ///< the original's blit alpha of 112, inverted
    static constexpr Color kFrozenTint = Color::rgba(0x80, 0x80, 0xFF, kAlpha);

    /** Lays it out for a boss of `meter` at `left`, drawing from `textures`; false when the
     * boss has no meter to show. */
    bool bind(const HealthMeterDefinition& meter, TextureSet* textures, s32 left = 0);
    void clear();
    bool bound() const { return m_pieces > 0; }

    /** Eases the health shown toward `health` of `max` over `ticks`; `alive` false takes the
     * meter down, `frozen` tints it. */
    void update(s32 ticks, f32 health, f32 maxHealth, bool alive, bool frozen);
    f32 shown() const { return m_sim.shown; }
    bool showing() const { return bound() && m_sim.alive; }
    /** How wide each strip's fill is drawn, in pixels of the virtual screen. */
    std::array<s32, kMostPieces> fillWidths() const;
    Look look() const { return {showing(), m_sim.frozen, fillWidths()}; }
    void preload(RenderDevice& device) const;

    void draw(Canvas& canvas, RenderDevice& device) const;
    /** Draws an authoritative appearance without advancing local easing or health. */
    void draw(Canvas& canvas, RenderDevice& device, const Look& look) const;

private:
    TextureSet* m_textures = nullptr;
    std::string m_name;
    s32 m_pieces = 0;
    s32 m_left = 0;
    s32 m_leftInset = 0;
    s32 m_rightInset = 0;
    bool m_backed = false;
    struct Simulation {
        f32 max = 1.0f;
        f32 shown = 0.0f;
        bool fresh = true; ///< nothing shown yet: the first health is taken as it is
        bool alive = true;
        bool frozen = false;
        bool operator==(const Simulation&) const = default;
        friend void visitNativeState(NativeStateDigest& digest, const Simulation& value) {
            digest.fields<5>(value);
        }
    };

public:
    class LocalSnapshot {
        friend class BossMeter;
        Simulation m_values;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const;
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    void appendValuesDigest(NativeStateDigest& digest, const Simulation& values) const;
    Simulation m_sim;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    u64 m_localEpoch = 1;
};

/** Ordered boss HUD layers: unbacked head fills share the preceding background's origin. */
class BossMeters {
public:
    BossMeters() = default;
    BossMeters(const BossMeters&) = delete;
    BossMeters& operator=(const BossMeters&) = delete;
    BossMeters(BossMeters&&) = delete;
    BossMeters& operator=(BossMeters&&) = delete;
    ~BossMeters() = default;
    static constexpr usize kCapacity = 3;
    class LocalSnapshot {
        friend class BossMeters;
        std::array<BossMeter::LocalSnapshot, kCapacity> m_meters;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const;
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    void bind(std::span<const HealthMeterReading> readings, TextureSet* textures);
    void update(s32 ticks, std::span<const HealthMeterReading> readings, bool alive, bool frozen);
    void clear();
    bool bound() const { return m_count > 0; }
    bool showing() const { return bound() && m_meters[0].showing(); }
    usize count() const { return m_count; }
    const BossMeter& meter(usize index) const { return m_meters.at(index); }
    void draw(Canvas& canvas, RenderDevice& device) const;

private:
    bool appendValuesDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    std::array<BossMeter, kCapacity> m_meters;
    usize m_count = 0;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    u64 m_localEpoch = 1;
};

} // namespace gdl::game
