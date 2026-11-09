#pragma once

#include "engine/assets/WorldData.h"

namespace gdl::game {

/** InitCameras (8c07bd8a..be04) replaces unauthored CAMS bounds before
 * either transmitter or boss cameras use them. The zero limits byte is not
 * an origin-only clamp; it selects the native world box with these margins. */
inline LevelCameraInfo resolveDreamcastCameraBounds(LevelCameraInfo camera, Vec3 worldMin,
                                                    Vec3 worldMax) {
    if (!camera.authoredBounds) {
        camera.boundsMin = worldMin + Vec3{8, 0, 8};
        camera.boundsMax = worldMax + Vec3{-8, 4, -8};
    }
    return camera;
}

} // namespace gdl::game
