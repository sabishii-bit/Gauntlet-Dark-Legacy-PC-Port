#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"

#include "engine/math/PortableMath.h"

namespace gdl::game {

Mat4 legendsDreamcastItemPlacement(const Vec3& position, const Vec3& rotation) {
    // InitItems (8c0a9b20) passes the unmodified record angles to 8c0a9920,
    // which builds this matrix with 8c0ece60. SetItem copies it unchanged
    // into the drawable. DrawTree's 8c064da2 concatenates its columns without
    // transposing them: T * Rx(-pitch) * Ry(yaw) * Rz(-roll).
    Mat4 transform = glm::translate(Mat4{1}, position);
    transform = portable::rotate(transform, -rotation.x, Vec3{1, 0, 0});
    transform = portable::rotate(transform, rotation.y, Vec3{0, 1, 0});
    return portable::rotate(transform, -rotation.z, Vec3{0, 0, 1});
}

} // namespace gdl::game
