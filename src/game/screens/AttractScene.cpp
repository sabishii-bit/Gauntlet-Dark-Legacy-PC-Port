#include "game/screens/AttractScene.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/AmbientOcclusion.h"

namespace gdl::game {
namespace {
constexpr f32 kRailSpeed = 12.0f;
constexpr f64 kMaxSeconds = 60.0;
constexpr f64 kInputDelay = 1.0;
WorldCamera cameraOf(const WorldLocator& marker) {
    return WorldCamera{marker.position, marker.rotation.x, marker.rotation.y, marker.rotation.z};
}
f32 blendAngle(f32 from, f32 to, f32 fraction) {
    return from + std::remainder(to - from, glm::two_pi<f32>()) * fraction;
}
} // namespace

bool AttractCamera::start(std::span<const WorldLocator> markers) {
    m_remaining.clear();
    m_finished = true;
    m_interpolate = false;
    m_hold = 0.0f;
    const WorldLocator* start = nullptr;
    for (const auto& marker : markers) {
        if (marker.kind == LocatorKind::CameraAttractStart && start == nullptr) {
            start = &marker;
        }
        if (marker.kind == LocatorKind::CameraAttract) {
            m_remaining.push_back(marker);
        }
    }
    if (start == nullptr || m_remaining.empty()) {
        return false;
    }
    m_camera = cameraOf(*start);
    m_previousCamera = m_camera;
    m_finished = false;
    selectNext();
    return true;
}

void AttractCamera::selectNext() {
    if (m_remaining.empty()) {
        m_hold = 0.5f;
        return;
    }
    const auto nearest =
        std::ranges::min_element(m_remaining, {}, [this](const WorldLocator& point) {
            return glm::distance(point.position, m_camera.position);
        });
    m_target = *nearest;
    m_remaining.erase(nearest);
    m_from = m_camera;
    m_distance = glm::distance(m_camera.position, m_target.position);
    m_travelled = 0.0f;
}

void AttractCamera::update(f32 seconds) {
    m_previousCamera = m_camera;
    m_interpolate = seconds > 0.0f;
    f32 left = std::max(0.0f, seconds);
    while (!m_finished && left > 0.0f) {
        if (m_hold > 0.0f) {
            m_hold -= left;
            m_finished = m_hold <= 0.0f;
            return;
        }
        const f32 time = (m_distance - m_travelled) / kRailSpeed;
        const f32 step = std::min(left, time);
        left -= step;
        m_travelled += step * kRailSpeed;
        const f32 fraction = m_distance > 0.0f ? std::min(m_travelled / m_distance, 1.0f) : 1.0f;
        // Coincident markers can change orientation instantaneously; never smooth that cut.
        if (m_distance == 0.0f) {
            m_interpolate = false;
        }
        m_camera.position = glm::mix(m_from.position, m_target.position, fraction);
        m_camera.pitch = blendAngle(m_from.pitch, m_target.rotation.x, fraction);
        m_camera.yaw = blendAngle(m_from.yaw, m_target.rotation.y, fraction);
        m_camera.roll = blendAngle(m_from.roll, m_target.rotation.z, fraction);
        if (step >= time) {
            selectNext();
        } else {
            break;
        }
    }
}

WorldCamera AttractCamera::presentedCamera(f32 alpha) const {
    return alpha >= 0.0f && m_interpolate ? m_camera.interpolate(m_previousCamera, alpha)
                                          : m_camera;
}

bool AttractScene::openNext(RenderDevice& device, const GameContext& context) {
    close();
    if (context.levels == nullptr) {
        return false;
    }
    m_context = context;
    const auto& realms = context.levels->realms();
    m_nextLevel.resize(realms.size());
    for (usize attempt = 0; attempt < realms.size(); ++attempt) {
        const usize realmIndex = m_nextRealm++ % realms.size();
        const auto& realm = realms[realmIndex];
        WorldData data;
        if (!data.load(context.unpackedRoot / "wdata" / (realm.file + ".json"))) {
            continue;
        }
        for (usize n = 0; n < realm.levels.size(); ++n) {
            const usize index = m_nextLevel[realmIndex]++ % realm.levels.size();
            const auto* info = data.level(realm.levels[index]);
            if (info == nullptr || (info->selectionFlags & 2U) == 0) {
                continue;
            }
            const auto level = context.levels->byName(realm.levels[index]);
            if (!level || !LevelCatalog::unpacked(context.unpackedRoot, *level)) {
                continue;
            }
            WorldLayout layout;
            if (!layout.load(context.unpackedRoot / level->directory) ||
                !m_rail.start(layout.locators())) {
                continue;
            }
            if (!m_world.load(device, context.unpackedRoot, *level)) {
                continue;
            }
            m_world.setPlayerCount(1);
            m_audio.open(context.unpackedRoot, context.sounds, m_world.audio(),
                         m_world.ref().name.empty() ? 'L' : m_world.ref().name.front(),
                         m_world.level() != nullptr && m_world.level()->bossType >= 0);
            m_audio.bindAmbience(m_world.layout(), &m_world.scene());
            // ItemVisible uses two players for attract flybys, not one camera ear.
            m_audio.setPlayerCount(2);
            m_audio.startMusic(context.assets, info->musicVolume);
            m_textures.load(context.unpackedRoot / "STATIC");
            if (m_font.load(context.unpackedRoot / "fonts/font32.json", 16)) {
                if (const auto font = m_textures.find("FONT32"); font.has_value()) {
                    m_text.setFont(&m_font, &m_textures.texture(device, *font));
                }
                if (const auto glow = m_textures.find("FONT32_GLOW"); glow.has_value()) {
                    m_glow = &m_textures.texture(device, *glow);
                }
            }
            m_elapsed = 0.0;
            m_open = true;
            log::info("Attract flyby: {}", level->name);
            return true;
        }
    }
    log::warn("No unpacked attract level with an authored camera rail is available");
    return false;
}

void AttractScene::close() {
    m_audio.close();
    m_world.clear();
    m_text = {};
    m_glow = nullptr;
    m_textures.releaseTextures();
    m_open = false;
    m_elapsed = 0.0;
}

AttractOutcome AttractScene::update(f64 seconds, const MenuInput& input) {
    if (!m_open) {
        return AttractOutcome::Finished;
    }
    m_elapsed += std::max(0.0, seconds);
    if (m_elapsed > kInputDelay && (input.start || input.select)) {
        return AttractOutcome::Title;
    }
    m_world.capturePresentation();
    m_rail.update(static_cast<f32>(seconds));
    m_world.update(static_cast<f32>(seconds));
    const Vec3 eye = m_rail.camera().position;
    const AmbientEar ear{eye, m_rail.camera().right()};
    m_audio.updateAmbience(std::span<const Vec3>(&eye, 1), ear,
                           m_world.level() != nullptr ? m_world.level()->soundVolume : 1.0f, false,
                           &m_world.scene());
    return m_rail.finished() || m_elapsed >= kMaxSeconds ? AttractOutcome::Finished
                                                         : AttractOutcome::Running;
}

void AttractScene::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height,
                          f32 presentationAlpha) {
    if (!m_open) {
        return;
    }
    const f32 fov = glm::radians(
        m_context.config != nullptr ? m_context.config->camera.horizontalFovDegrees : 60.0f);
    const WorldCamera camera = m_rail.presentedCamera(presentationAlpha);
    const Mat4 clip = camera.clipTransform(fov, width, height, projection);
    m_world.drawOpaque(device, clip, camera, presentationAlpha);
    if (m_context.config != nullptr && m_context.config->display.ambientOcclusion) {
        AmbientOcclusion occlusion;
        occlusion.clipToView = camera.view() * glm::inverse(clip);
        device.applyAmbientOcclusion(occlusion);
    }
    m_world.drawDeferred(device, clip, camera, presentationAlpha);
    if (m_context.config != nullptr && m_context.config->display.bloom) {
        device.applyBloom();
    }
}
} // namespace gdl::game
