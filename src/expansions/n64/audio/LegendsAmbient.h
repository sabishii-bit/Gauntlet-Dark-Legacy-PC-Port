#pragma once

#include <span>
#include <vector>

#include "expansions/n64/formats/LegendsN64Ambient.h"

namespace gdl::game {

struct LegendsAmbientVoice {
    u32 nativeIndex = 0;
    u16 token = 0xffff;
    Vec3 position{0};
    f32 radius = 0;
    f32 volume = 1;
};

/** Native desired ambient voices. The scene retains actual SoundPlayer handles;
 * the native macro owns its looping, so do not restart a decoded sample at EOF.
 * Supply the camera attention point, not its eye or an interpolated render pose. */
class LegendsAmbient {
public:
    bool reset(std::span<const formats::LegendsN64Ambient> definitions);
    std::vector<LegendsAmbientVoice> update(const Vec3& cameraTarget, bool departing = false);
    usize size() const { return m_emitters.size(); }

private:
    struct Emitter {
        formats::LegendsN64Ambient definition;
        f32 previousGain = 1;
        f32 volume = 1;
        bool active = false;
    };
    std::vector<Emitter> m_emitters;
};

} // namespace gdl::game
