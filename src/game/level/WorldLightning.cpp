#include "game/level/WorldLightning.h"

#include <cmath>
#include <string>
#include <utility>

#include "engine/assets/WorldLayout.h"
#include "engine/math/RandomDistribution.h"
#include "engine/world/WorldScene.h"

#include "game/level/SimulationSeed.h"

namespace gdl::game {

namespace {
// world_update (0x8005674C): a sheet shows for kFlashSeconds (0x80346C58), then stays hidden
// for kMinimumWaitSeconds (0x80346C60) plus Random(kWaitRangeSeconds) (0x80346C68).
constexpr f32 kFlashSeconds = 0.2f;
constexpr f32 kMinimumWaitSeconds = 5.0f;
constexpr f32 kWaitRangeSeconds = 10.0f;

void applyFlash(WorldScene& scene, const WorldLightning::Flash& flash) {
    scene.setObjectVisible(flash.object, flash.visible);
    for (const usize child : flash.children) {
        scene.setObjectVisible(child, flash.visible);
    }
}
} // namespace

void WorldLightning::bind(const WorldLayout& layout, std::string_view stage) {
    clear();
    const std::string_view prefix = stage == "A5"   ? "A5XPXLNC_LNING"
                                    : stage == "F2" ? "F2XPNC_LNING"
                                                    : "";
    if (prefix.empty()) {
        return;
    }
    m_random.seed(simulationSeed(stage));
    // InitWorld (fn_80057024) selects exactly these four names, not all
    // additive/sort-behind materials. Other glows and forcefields stay alone.
    for (char number = '1'; number <= '4'; ++number) {
        const std::string name = std::string(prefix) + number;
        for (usize object = 0; object < layout.objects().size(); ++object) {
            if (layout.objects()[object].name == name) {
                Flash flash{object, {}, waitSeconds(), false};
                // The named root is only a group. In the shipped A5/F2 data
                // the actual two-triangle bolt is two children below it.
                // MBTreeSetFlags hides that complete tree, not just its root.
                for (usize child = 0; child < layout.objects().size(); ++child) {
                    s32 parent = layout.objects()[child].parent;
                    for (usize guard = 0;
                         parent >= 0 && static_cast<usize>(parent) < layout.objects().size() &&
                         guard < layout.objects().size();
                         ++guard) {
                        if (static_cast<usize>(parent) == object) {
                            flash.children.push_back(child);
                            break;
                        }
                        parent = layout.objects()[static_cast<usize>(parent)].parent;
                    }
                }
                m_flashes.push_back(std::move(flash));
                break;
            }
        }
    }
}

void WorldLightning::clear() {
    m_flashes.clear();
    m_random.seed(1);
}

f32 WorldLightning::waitSeconds() {
    // A dedicated portable stream keeps this presentation script independent
    // of enemy spawning and draw cadence.
    constexpr u32 kRange = 0x1000000;
    return kMinimumWaitSeconds +
           static_cast<f32>(*random_distribution::boundedIndex(m_random, kRange)) *
               (kWaitRangeSeconds / static_cast<f32>(kRange));
}

void WorldLightning::apply(WorldScene& scene) const {
    for (const Flash& flash : m_flashes) {
        applyFlash(scene, flash);
    }
}

void WorldLightning::update(f32 seconds, WorldScene& scene) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    // world_update (8005674c) has no Stop Time gate here. Pausing/modal
    // dialogue holds the world clock; ordinary entrance camera pans do not.
    for (Flash& flash : m_flashes) {
        flash.remaining -= seconds;
        if (flash.remaining <= 0) {
            flash.visible = !flash.visible;
            // The native deadline is relative to the update that switches
            // visibility, so a late update still shows the full flash.
            flash.remaining = flash.visible ? kFlashSeconds : waitSeconds();
            applyFlash(scene, flash);
        }
    }
}

} // namespace gdl::game
