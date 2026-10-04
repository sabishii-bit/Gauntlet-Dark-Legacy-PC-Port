#pragma once
#include <optional>

#include "engine/world/WorldCollision.h"

#include "game/players/PlayerControls.h"

namespace gdl::game {
/** Unproject through the actual rendered camera, then pick solid level geometry. */
std::optional<Vec3> cursorAimPoint(Vec2 cursor, const Mat4& clip, const WorldCollision& collision,
                                   f32 fallbackHeight);
/** Transform forward/back/strafe input from the cursor-facing basis into camera input space. */
MoveInput cursorRelativeMove(MoveInput move, const Vec3& position, const Vec3& aim, f32 cameraYaw);
/** Automatic profiles retain controller aiming until keyboard/mouse activity takes over. */
class CursorInput {
public:
    bool update(const Input& input, const PlayBindings& bindings, bool keyboard, s32 pad);

private:
    std::optional<Vec2> m_previous;
    bool m_active = false;
};
} // namespace gdl::game
