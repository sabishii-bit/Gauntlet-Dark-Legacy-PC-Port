#pragma once
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/world/WorldCamera.h"

#include "game/camera/FlightVelocity.h"
#include "game/players/PlayerControls.h"

namespace gdl::game {

/**
 * How a character flies when the developer console lets it through everything (noclip): the
 * movement keys or the left stick take it along the ground the way the camera looks, Q and E
 * or the right stick straight up and down, Shift slowing it, the velocity easing the way the
 * free camera's does. The scene moves the character by the eased velocity flown, facing the
 * way it goes along the ground.
 */
class NoclipMotion {
public:
    static constexpr f32 kSpeed = 50.0f;       ///< units a second, four times a quick run
    static constexpr f32 kSlowFactor = 0.25f;  ///< with Shift held
    static constexpr f32 kRiseDeadZone = 0.2f; ///< of the right stick's throw
    static constexpr std::string_view kHelp =
        "the movement keys or left stick fly you as the camera looks, Q and E or the right "
        "stick up and down, Shift slows";

    /** What is asked: the way to go, up to a unit's length, and whether slowly. */
    struct Wish {
        Vec3 direction{0.0f, 0.0f, 0.0f};
        bool slow = false;
        bool operator==(const Wish&) const = default;
    };
    /** Reads the wish from the keyboard (when `keyboard`) and the pad `pad` (as
     * readMoveInput takes them), along the ground as `camera` looks. */
    static Wish wish(const Input& input, const PlayBindings& bindings, bool keyboard, s32 pad,
                     const WorldCamera& camera);

    /** Eases toward `wish` over `seconds` and returns the flight wish the scene integrates:
     * the eased velocity over kSpeed, each axis within -1..1. */
    Vec3 fly(const Wish& wish, f32 seconds);
    void reset() { m_velocity.reset(); }
    const Vec3& velocity() const { return m_velocity.velocity; }

private:
    FlightVelocity m_velocity;
};

} // namespace gdl::game
