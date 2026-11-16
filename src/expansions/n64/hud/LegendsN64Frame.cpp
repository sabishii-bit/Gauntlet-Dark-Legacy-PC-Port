#include "expansions/n64/hud/LegendsN64Frame.h"

#include "engine/math/Math.h"

namespace gdl::game {

Vec2 LegendsN64Frame::toCanvas(Vec2 frame) {
    return {kLeft + frame.x * kColumn, kTop + frame.y * kRow};
}

Vec2 LegendsN64Frame::toFrame(Vec2 canvas) {
    return {(canvas.x - kLeft) / kColumn, (canvas.y - kTop) / kRow};
}

Mat4 LegendsN64Frame::transform() {
    return glm::scale(glm::translate(Mat4{1.0f}, Vec3{kLeft, kTop, 0.0f}),
                      Vec3{kColumn, kRow, 1.0f});
}

} // namespace gdl::game
