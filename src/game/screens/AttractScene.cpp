#include "game/screens/AttractScene.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/menu/OptionMenu.h"

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
            m_audio.open(context.unpackedRoot, context.sounds, m_world.audio());
            m_audio.bindAmbience(m_world.layout());
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
    m_rail.update(static_cast<f32>(seconds));
    m_world.update(static_cast<f32>(seconds));
    const Vec3 eye = m_rail.camera().position;
    const AmbientEar ear{eye, m_rail.camera().right()};
    m_audio.updateAmbience(std::span<const Vec3>(&eye, 1), ear,
                           m_world.level() != nullptr ? m_world.level()->soundVolume : 1.0f);
    return m_rail.finished() || m_elapsed >= kMaxSeconds ? AttractOutcome::Finished
                                                         : AttractOutcome::Running;
}

void AttractScene::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    if (!m_open) {
        return;
    }
    const f32 fov = glm::radians(
        m_context.config != nullptr ? m_context.config->camera.horizontalFovDegrees : 60.0f);
    const WorldCamera& camera = m_rail.camera();
    m_world.draw(device, camera.clipTransform(fov, width, height, projection), camera);
    if (m_context.strings != nullptr) {
        m_canvas.begin(device,
                       makeVirtualScreenTransform(projection, 512.0f, 384.0f, width, height));
        const auto label = m_context.strings->get("title.pressStart");
        TextStyle glow;
        glow.color = Color::rgba(130, 0, 234)
                         .withAlpha(pulseOpacity(static_cast<s32>(m_elapsed * 60.0), 40, 5));
        glow.texture = m_glow;
        glow.expand = OptionMenu::kGlowExpand;
        m_text.draw(m_canvas, -256, 320, label, glow);
        m_text.draw(m_canvas, -256, 320, label, TextStyle{});
        m_canvas.end();
    }
}
} // namespace gdl::game
