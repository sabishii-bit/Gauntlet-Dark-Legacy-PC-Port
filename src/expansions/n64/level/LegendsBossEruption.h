#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "expansions/shared/formats/LegendsMap.h"

namespace gdl::game {

/** Imported Skorne entrance compatibility, not a recovered N64 script callback.
 * Shows the native ROCKFLY floor cap at rest, following GDL's entrance contract,
 * then uses native fly-effect motion. The permanent floor/collision is unchanged. */
class LegendsBossEruption {
public:
    static constexpr f64 kLifetime = 375.0 / 150.0;
    static constexpr usize kMaximumFragments = 128;
    LegendsBossEruption() = default;
    explicit LegendsBossEruption(std::span<const formats::LegendsMapObject> objects);
    bool begin();
    void capturePresentation();
    void update(f32 seconds);
    std::optional<bool> visible(usize object) const;
    Mat4 transform(usize object, f32 alpha = -1) const;
    Vec3 offset(usize object, f32 alpha = -1) const;
    usize size() const { return m_fragments.size(); }

private:
    struct Pose {
        f32 height = 0;
        Quat rotation{1, 0, 0, 0};
    };
    struct Fragment {
        usize object = 0;
        Vec3 anchor{0};
        std::array<Pose, 76> poses;
    };
    const Fragment* find(usize object) const;
    Pose sample(const Fragment& fragment, f32 alpha) const;
    std::vector<Fragment> m_fragments;
    f64 m_age = 0;
    f64 m_previousAge = 0;
    bool m_started = false;
};

} // namespace gdl::game
