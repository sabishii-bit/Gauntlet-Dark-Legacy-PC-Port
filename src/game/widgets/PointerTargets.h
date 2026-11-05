#pragma once

#include <algorithm>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** The mouse controls a screen shows, in its canvas coordinates, registered where it lays
 * them out; where two overlap, the one registered last is on top. */
template <class Control> class PointerTargets {
public:
    /** One control and the area it answers in. */
    struct Target {
        Rect area;
        Control control;
    };

    /** Forgets the controls shown before; called as the screen lays them out again. */
    void clear() { m_targets.clear(); }

    /** Shows `control` over `area`, cut to `bounds` when given; a cut with nothing left shows
     * nothing. */
    void add(const Rect& area, const Control& control, const std::optional<Rect>& bounds = {}) {
        if (!bounds) {
            m_targets.push_back({area, control});
            return;
        }
        const f32 left = std::max(area.x, bounds->x);
        const f32 top = std::max(area.y, bounds->y);
        const f32 right = std::min(area.right(), bounds->right());
        const f32 bottom = std::min(area.bottom(), bounds->bottom());
        if (right > left && bottom > top) {
            m_targets.push_back({Rect{left, top, right - left, bottom - top}, control});
        }
    }

    /** The topmost target under `point` that `accepts` takes, or nothing. */
    template <class Accepts> const Target* at(Vec2 point, const Accepts& accepts) const {
        for (const Target& target : m_targets | std::views::reverse) {
            if (target.area.contains(point) && accepts(target)) {
                return &target;
            }
        }
        return nullptr;
    }

    /** The topmost target under `point`, or nothing. */
    const Target* at(Vec2 point) const {
        return at(point, [](const Target& /*target*/) { return true; });
    }

    /** Every target shown, lowest first. */
    std::span<const Target> targets() const { return m_targets; }

private:
    std::vector<Target> m_targets;
};

/** Whether a pointer at `now` has moved more than `threshold` from `last`, where it last
 * chose; a pointer that has just appeared always has. */
bool pointerMoved(const std::optional<Vec2>& last, Vec2 now, f32 threshold);

} // namespace gdl::game
