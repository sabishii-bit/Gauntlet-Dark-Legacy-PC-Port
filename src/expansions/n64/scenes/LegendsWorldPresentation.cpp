#include "expansions/n64/scenes/LegendsWorldPresentation.h"

#include <cmath>

namespace gdl::game {

LegendsWorldPresentation LegendsWorldPresentation::n64(u32 flags, u32 worldFlags) {
    LegendsWorldPresentation result;
    // e0027bf0 skips hidden nodes. e001e794 checks sorted eligibility rather
    // than treating every alpha texture as a deferred object. e001ea34 selects
    // its translucent RDP render mode for 0x10000000, and independently clears
    // Z_UPD/Z_CMP for 0x80/0x40; 0x1000 bypasses RSP normal lighting.
    result.hidden = (flags & 2U) != 0;
    result.sortable = (flags & 0x800U) != 0;
    result.forceAlpha = (flags & 0x10000000U) != 0;
    result.depthWrite = (flags & 0x80U) == 0;
    result.depthTest = (flags & 0x40U) == 0;
    result.unlit = (flags & 0x1000U) != 0;
    result.facing = (flags >> 24U) & 7U;
    // e0039370 selects the custom static-world path with this exact predicate.
    // Other WOBJ records become drawables in e003955c, which maps world bit
    // 0x10000000 to drawable bit 0x10000. Static records instead modify a
    // homogeneous matrix coefficient; do not apply the drawable rule to them.
    const bool staticWorld = (worldFlags & 0x5001U) == 1 && (flags & 0x07000c00U) == 0;
    result.cameraDepthPull =
        (flags & 0x10000U) != 0 || ((worldFlags & 0x10000000U) != 0 && !staticWorld);
    return result;
}

Mat4 LegendsWorldPresentation::placement(const Mat4& authored, const WorldCamera& camera) const {
    if (facing != 1) {
        return authored;
    }
    return CameraFrame::of(camera).face(authored, CameraFrame::kFacingProjectedYaw);
}

Mat4 LegendsWorldPresentation::drawAdjustment(const WorldCamera& camera) const {
    if (!cameraDepthPull) {
        return Mat4{1};
    }
    const Vec3 backward = -camera.forward();
    if (!std::isfinite(backward.x) || !std::isfinite(backward.y) || !std::isfinite(backward.z)) {
        return Mat4{1};
    }
    // e0027f80 rotates (0,0,-1) by the camera basis and truncates each component
    // times 4096. e001ea34/e002c0f8 add that vector times DAT_e001f6f4 (0x300)
    // to the 16.16 drawable translation. Native world drawables use half-size
    // translation (e003955c), hence 0x300 / (65536 * 32) in our world units.
    // This is a camera-directed presentation offset, not a world-Y nudge or a
    // disabled Z test. Nearer solid geometry must continue to occlude decals.
    Vec3 offset;
    for (usize axis = 0; axis < 3; ++axis) {
        offset[axis] = std::trunc(backward[axis] * 4096.0f) * (3.0f / 8192.0f);
    }
    return glm::translate(Mat4{1}, offset);
}

} // namespace gdl::game
