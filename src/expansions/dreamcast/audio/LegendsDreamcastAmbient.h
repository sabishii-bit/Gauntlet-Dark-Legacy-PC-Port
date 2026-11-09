#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"

namespace gdl::game {

/** Native type-13 sound markers. This is a presentation query, not mixer state:
 * the caller owns playback, pause/resume, and the optional moving source pose. */
class LegendsDreamcastAmbient {
public:
    struct Emitter {
        u32 nativeIndex = 0;
        u32 token = 0;
        Vec3 position{0};
        f32 radius = 0;
        u16 flags = 0;
    };
    struct Request {
        u32 nativeIndex = 0;
        u32 token = 0;
        Vec3 position{0};
        f32 loudness = 0;
        f32 volume = 0;
        u16 flags = 0;
        std::optional<f32> musicScale;
    };
    using TokenResolver = std::function<std::optional<u32>(std::string_view)>;
    using PositionResolver = std::function<std::optional<Vec3>(u32)>;
    static constexpr f32 kMusicDuckHold = .5f;

    /** Missing audio and music-zone markers do not manufacture replacement loops. */
    bool reset(const formats::LegendsDreamcastScene& scene, s32 partyCount,
               const TokenResolver& resolve);
    /** Native do_items selects one nearest audible source using camera attention,
     * not camera eye or player position. Equal-distance sources retain file order. */
    std::optional<Request> request(const Vec3& attention,
                                   const PositionResolver& positions = {}) const;
    std::span<const Emitter> emitters() const { return m_emitters; }

private:
    std::vector<Emitter> m_emitters;
};

} // namespace gdl::game
