#pragma once

#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {
class WorldLayout;
class WorldScene;
} // namespace gdl

namespace gdl::game {

/** Scripted background sheets in the Chimera and Throne of Skorne arenas.
 * These have no authored animation track: InitWorld hides four named trees,
 * and world_update briefly reveals each after its own random wait. All values
 * live with the world's simulation so a local rewind restores the next flash
 * as well as the current visibility. No rendering or gameplay RNG is borrowed. */
class WorldLightning {
public:
    struct Flash {
        usize object = 0;
        std::vector<usize> children;
        f32 remaining = 0;
        bool visible = false;
        bool operator==(const Flash&) const = default;
        template <class Digest> friend void visitNativeState(Digest& digest, const Flash& value) {
            digest.template fields<4>(value);
        }
    };

    bool operator==(const WorldLightning&) const = default;
    template <class Digest>
    friend void visitNativeState(Digest& digest, const WorldLightning& value) {
        digest.values(value.m_flashes, value.m_random);
    }

    void bind(const WorldLayout& layout, std::string_view stage);
    void clear();
    std::span<const Flash> flashes() const { return m_flashes; }
    /** Apply only after the selected sheets have been built as controlled units. */
    void apply(WorldScene& scene) const;
    void update(f32 seconds, WorldScene& scene);

private:
    f32 waitSeconds();
    std::vector<Flash> m_flashes;
    std::minstd_rand m_random{1};
};

} // namespace gdl::game
