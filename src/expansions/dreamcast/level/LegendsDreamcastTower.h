#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/world/WorldCollision.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "expansions/shared/formats/LegendsMap.h"

namespace gdl::game {

/** Dreamcast TowerInit (8c065860). Only native tower objects are bound;
 * collection bits belong to the Dreamcast campaign, never the GC/N64 domains. */
class LegendsDreamcastTower {
public:
    explicit LegendsDreamcastTower(const formats::LegendsMap& map);
    void synchronize(const LegendsProgress& progress) { m_progress = progress; }
    bool visible(usize object) const;
    void syncCollision(WorldCollision& collision) const;
    bool gateOpen(usize gate) const;

    /** The four native Sumner topics in 8c065e60, returning localization keys. */
    static std::vector<std::string> advice(s32 topic, const LegendsProgress& progress);

private:
    enum class Kind { Gate, Rune, Shard };
    struct Binding {
        usize object;
        Kind kind;
        usize index;
    };
    std::vector<Binding> m_bindings;
    LegendsProgress m_progress;
};

struct DreamcastSumnerVisitor {
    s32 seat = -1;
    Vec3 position{0};
    bool interactPressed = false;
};

/** Native proximity area and facing, admitting only a fresh interaction press.
 * No pointers into players or assets are retained. */
class LegendsDreamcastSumner {
public:
    std::optional<s32> update(std::span<const DreamcastSumnerVisitor> visitors, f32 seconds,
                              bool blocked);
    f32 yaw() const { return m_yaw; }
    static constexpr Vec3 kPosition{0, 0, 16};

private:
    f32 m_yaw = kPi;
};

} // namespace gdl::game
