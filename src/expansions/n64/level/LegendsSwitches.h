#pragma once

#include <span>
#include <string>
#include <vector>

#include "expansions/n64/formats/LegendsN64Switches.h"
#include "expansions/shared/level/LegendsPortalVisitor.h"

namespace gdl::game {

bool touchesLegendsSwitch(const formats::LegendsN64Switch& definition,
                          const LegendsPortalVisitor& visitor);

/** Native lift/breakaway motion, independent of renderer/collision ownership.
 * The bridge resolves target names once and applies the offsets to that WOBJ's
 * render subtree and owned collision, never to unrelated same-named models. */
class LegendsSwitches {
public:
    struct Target {
        std::string name;
        f32 first = 0;
        f32 second = 0;
        f32 offset = 0; ///< native collision displacement, in imported world units
        f32 previous = 0;
        bool activated = false;
        bool moving = false;
        f32 goal = 0; ///< native zero-initialized destination, independent of first
        f64 lastMotion = 0;
        u8 occupants = 0;
        bool fading = false; ///< offset is native transparency phase, never Y displacement
        bool requested = false;
        bool supported = false; ///< a living actor currently stands on this target
        f32 sample(f32 alpha = -1) const;
    };
    struct Switch {
        formats::LegendsN64Switch definition;
        usize target = 0;
        bool activated = false;
    };

    explicit LegendsSwitches(std::span<const formats::LegendsN64Switch> definitions = {},
                             u8 activationThreshold = 1);
    void capturePresentation();
    /** Returns true on admission. One-shot switches never re-arm. */
    bool activate(u32 nativeIndex);
    /** Current living contact mask. Repeat switches return to idle on release;
     * their shared platform retains its last requested endpoint. */
    void contact(u32 nativeIndex, u8 occupants);
    void support(std::span<const usize> targets);
    void update(f32 seconds);
    std::span<const Target> targets() const { return m_targets; }
    std::span<const Switch> switches() const { return m_switches; }

private:
    std::vector<Target> m_targets;
    std::vector<Switch> m_switches;
    f64 m_time = 0;
    void request(Switch& plate, u8 occupants);
};

} // namespace gdl::game
