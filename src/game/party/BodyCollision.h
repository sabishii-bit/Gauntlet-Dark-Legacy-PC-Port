#pragma once

#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/MissileTarget.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {
class PlayerActor;
/** A great one's solid part or body as a player's collision centre meets it: about the
 * volume's middle, its radius and half height each widened by the player's width. */
struct ContactCylinder {
    Vec3 centre{0};
    f32 radius = 0;
    f32 halfHeight = 0;
};

/** Swept horizontal movement against the same upright volumes used for hit tests.
 * Does not move the obstacles or retain the supplied view. */
class BodyCollision {
public:
    static Vec3 resolve(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                        std::span<const MissileTarget> bodies);
    /** The same sweep over `bodies` and `cylinders` at once; a cylinder counts where its band
     * holds the step's collision centre, `centreHeight` over its feet. */
    static Vec3 resolve(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                        std::span<const MissileTarget> bodies, f32 centreHeight,
                        std::span<const ContactCylinder> cylinders);
    /** `volume` as a player `width` wide meets it (CritterMoveNodeCol, CritterExpNodeColSub). */
    static ContactCylinder critterContact(const MissileTarget& volume, f32 width);
    /** Adds `volumes`, great ones' solid parts and bodies or a boss's, to `cylinders` as
     * `player`'s collision centre meets them: each by the character's whole width. */
    static void addCritterContacts(std::vector<ContactCylinder>& cylinders,
                                   const PlayerActor& player,
                                   std::span<const MissileTarget> volumes);
    /** How high over its feet `player`'s collision centre stands. */
    static f32 contactHeight(const PlayerActor& player);
    /** Whether a step of the collision centre from `from` to `to` meets one of `cylinders` by
     * LineCylinderCollide's directional test (0x8002FA70): from inside one, any step but one
     * heading away from it meets it. */
    static bool touches(const Vec3& from, const Vec3& to,
                        std::span<const ContactCylinder> cylinders);
    /** Slide against authored item boxes, subdividing long steps to prevent tunnelling. */
    static Vec3 resolveItems(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                             std::span<const Obstacle> items);
    /** Native opponent/generator response, including static-world rechecks. The
     * ordered geometry is observational; this never changes any contacted body. The great
     * ones' solid parts and bodies, `critters`, meet the player's collision centre by its
     * whole width; the other bodies meet its footprint. */
    static Vec3 resolveMovement(const PlayerActor& player, const Vec3& from, const Vec3& to,
                                std::span<const MissileTarget> bodies,
                                std::span<const Obstacle> generators,
                                const WorldCollision* collision,
                                std::span<const MissileTarget> critters = {});
};
} // namespace gdl::game
