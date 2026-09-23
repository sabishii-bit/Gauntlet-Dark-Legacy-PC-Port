#pragma once
#include <optional>
#include <string>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
namespace gdl::game {
/** A contact a critter has made. Breath contacts repeat while touching; the
 * recipient's shared breath timer decides when they may damage it again. */
struct CombatBlow {
    s32 player = -1;
    s32 critter = -1;
    f32 damage = 0.0f;
    Vec3 direction{0.0f, 0.0f, 1.0f};
    bool breath = false;
    u32 flags = 0;     ///< authored player damage modifiers, not the attack's behavior flags
    Vec3 origin{0.0f}; ///< emitted segment origin, used for breath cover queries
    bool area = false;
    f32 repeatGap = 0.0f; ///< area-effect immunity requested on contact
};

/** An effect and a sound a critter has set off: a move's, a strike's or a hit's, where it
 * happened; `follows` for one that rides on the body. */
struct CombatCue {
    s32 critter = -1;
    std::string tree;  ///< of the critter's own archive; empty for a sound alone
    std::string sound; ///< empty for an effect alone
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
    f32 life = 0.0f; ///< seconds, when it does not play out
    bool follows = false;
    bool shakes = false;
    bool rootAttachment = false;     ///< root transform, not a fixed world-space body offset
    std::optional<std::string> node; ///< animated attachment, distinct from a body translation
    Vec3 nodeOffset{0.0f};
    Vec2 pitchYaw{0.0f}; ///< local effect rotation, independent of its attachment offset
    bool loop = true;
};

/** A dying critter's death throwing something out (the coins a boss spews): from where,
 * which way and how fast, and how far round each side of that way. */
struct CombatSpew {
    s32 critter = -1;
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    f32 halfAngle = 0.0f; ///< radians
};

/** Experience a critter is worth: a share of its value for each hit, to the hitter, and
 * a fifth of it to everyone (`player` -1) when it falls. */
struct CombatLoss {
    s32 critter = -1;
    s32 kind = 0;
    std::string form; ///< a gargoyle's ("EAGL"): the key it drops is named by it
    s32 player = -1;
    f32 experience = 0.0f;
    bool killed = false;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

} // namespace gdl::game
