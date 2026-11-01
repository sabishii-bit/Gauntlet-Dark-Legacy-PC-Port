#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

struct LegendsN64CameraMarker {
    Vec3 position{0};
    f32 yaw = 0;
    f32 pitch = 0;
};

struct LegendsN64CameraData {
    // IST entry zero supplies the opening orbit centre, not a nearest-marker
    // candidate. Native entry truncates that XYZ to whole units and takes its
    // orientation from the nearest ordinary marker, not marker zero's angles.
    std::vector<LegendsN64CameraMarker> markers;
    std::vector<Vec3> towerExits;
    bool tower = false;
    // Boss-stage e006b110 supplies a fixed arena focus in addition to the party.
    // It is not the moving model root (Dragon's root has a separate floor offset).
    std::optional<Vec3> bossFocus;
};

/** Reads the native IST camera table. Tower's radial camera additionally needs
 * its native realm-exit directions from the extracted US game.bin. */
LegendsN64CameraData decodeLegendsN64Camera(std::span<const u8> settings,
                                            std::string_view levelName,
                                            std::span<const u8> program = {});

} // namespace gdl::formats
