#include "game/screens/IdleScreen.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {
namespace {
constexpr std::array<std::string_view, SaverMotion::kCount> kNames{"SAVER_AXE", "SAVER_HAM",
                                                                   "SAVER_SWD", "SAVER_WND"};
constexpr std::array<Vec3, SaverMotion::kCount> kPositions{Vec3{20, 0, 1}, Vec3{-20, 0, 1},
                                                           Vec3{0, 20, 1}, Vec3{0, -20, 1}};
constexpr std::array<Vec3, SaverMotion::kCount> kDirections{Vec3{-1, 0, 1}, Vec3{1, 0, 1},
                                                            Vec3{0, -1, 1}, Vec3{0, 1, 1}};
constexpr f32 kSpeed = 10.0f;
constexpr f32 kNear = 5.0f;
constexpr f32 kFar = 40.0f;
constexpr f64 kTickSeconds = 1.0 / 60.0;
constexpr f32 kStickThreshold = 0.25f;
} // namespace

IdleWatch::Buttons IdleWatch::snapshot(const Input& input) {
    Buttons result;
    for (usize key = 1; key < kKeys; ++key) {
        result[key] = input.isKeyDown(static_cast<Key>(key));
    }
    for (s32 pad = 0; pad < Input::kMaxPads; ++pad) {
        const usize base = kKeys + static_cast<usize>(pad) * kPadBits;
        for (usize button = 0; button < kButtons; ++button) {
            result[base + button] = input.isPadButtonDown(pad, static_cast<PadButton>(button));
        }
        for (usize axis = 0; axis < kAxes; ++axis) {
            const f32 value = input.padAxis(pad, static_cast<PadAxis>(axis));
            result[base + kButtons + 2 * axis] = value > kStickThreshold;
            result[base + kButtons + 2 * axis + 1] = value < -kStickThreshold;
        }
    }
    return result;
}

bool IdleWatch::update(f64 seconds, const Input& input, bool eligible) {
    const Buttons current = snapshot(input);
    const bool changed = current != m_previous;
    m_previous = current;
    if (!eligible) {
        reset();
        return false;
    }
    if (m_active && changed) {
        m_active = false;
        m_releasing = true;
        m_seconds = 0.0;
        return false;
    }
    if (m_releasing) {
        m_releasing = current.any();
        m_seconds = 0.0;
        return false;
    }
    m_seconds = changed ? 0.0 : m_seconds + std::max(0.0, seconds);
    m_active = m_seconds >= kWaitSeconds;
    return m_active;
}

void IdleWatch::reset() {
    m_seconds = 0.0;
    m_active = false;
    m_releasing = false;
}

f32 SaverMotion::random(f32 range) {
    return static_cast<f32>(m_random() - std::minstd_rand::min()) /
           static_cast<f32>(std::minstd_rand::max() - std::minstd_rand::min()) * range;
}

void SaverMotion::start() {
    m_remainder = 0.0;
    for (usize i = 0; i < kCount; ++i) {
        auto& weapon = m_weapons[i];
        weapon = {};
        weapon.position = kPositions[i];
        weapon.delay = static_cast<s32>(i) * 30 + static_cast<s32>(random(15.0f)) + 1;
        weapon.resetAt = static_cast<s32>(600.0f * (0.5f + random(1.3f)));
    }
}

void SaverMotion::update(f64 seconds, f32 horizontalFov, f32 aspect) {
    m_remainder += std::max(0.0, seconds);
    while (m_remainder + 1.0e-9 >= kTickSeconds) {
        m_remainder -= kTickSeconds;
        step(horizontalFov, aspect);
    }
}

void SaverMotion::step(f32 horizontalFov, f32 aspect) {
    for (usize i = 0; i < kCount; ++i) {
        auto& weapon = m_weapons[i];
        if (++weapon.elapsed < weapon.delay) {
            continue;
        }
        if (weapon.delay > 0) {
            weapon.position = kPositions[i];
            weapon.velocity = glm::normalize(kDirections[i]);
            weapon.angle = 0.0f;
            weapon.collision = 0;
            weapon.delay = 0;
            weapon.visible = true;
        }
        weapon.position += weapon.velocity * (kSpeed / 60.0f);
        weapon.angle =
            std::remainder(weapon.angle - glm::two_pi<f32>() / 60.0f, glm::two_pi<f32>());
        const f32 halfWidth = weapon.position.z * std::tan(horizontalFov * 0.5f);
        const f32 halfHeight = halfWidth / std::max(0.1f, aspect);
        s32 collision = 0;
        if (weapon.position.z > kFar) {
            collision = 5;
        } else if (weapon.position.z < kNear) {
            collision = 6;
        } else if (weapon.position.x < -halfWidth) {
            collision = 1;
        } else if (weapon.position.x > halfWidth) {
            collision = 2;
        } else if (weapon.position.y > halfHeight) {
            collision = 3;
        } else if (weapon.position.y < -halfHeight) {
            collision = 4;
        }
        if (weapon.collision > 0 && collision != 0) {
            const f32 bounce = 0.5f + random(0.5f);
            const f32 spread = -0.1f + random(0.2f);
            if (collision <= 2) {
                weapon.velocity.x = collision == 1 ? bounce : -bounce;
                weapon.velocity.y += spread;
            } else if (collision <= 4) {
                weapon.velocity.y = collision == 3 ? -bounce : bounce;
                weapon.velocity.x += spread;
            } else {
                weapon.velocity.z = collision == 5 ? -bounce : bounce;
                if (collision == 5 && weapon.elapsed > weapon.resetAt) {
                    weapon.visible = false;
                    weapon.elapsed = 0;
                    weapon.delay = static_cast<s32>(180.0f * (0.5f + random(1.3f))) + 1;
                    weapon.resetAt = static_cast<s32>(600.0f * (0.5f + random(1.3f)));
                }
            }
            weapon.velocity = glm::normalize(weapon.velocity);
            weapon.collision = -10;
        } else if (weapon.collision < 0 || collision == 0) {
            weapon.collision = 1;
        }
    }
}

bool IdleScreen::open(RenderDevice& device, const std::filesystem::path& root) {
    close();
    if (!m_archive.load(root / "POWERUPS")) {
        return false;
    }
    for (const auto name : kNames) {
        if (!m_archive.trees.find(name).has_value()) {
            close();
            return false;
        }
    }
    m_device = &device;
    m_motion.start();
    m_open = true;
    return true;
}

void IdleScreen::close() {
    m_effects.clear();
    m_handles.fill(0);
    m_archive.clear();
    m_device = nullptr;
    m_open = false;
}

void IdleScreen::update(f64 seconds, f32 horizontalFov, f32 aspect) {
    if (!m_open) {
        return;
    }
    m_motion.update(seconds, horizontalFov, aspect);
    for (usize i = 0; i < SaverMotion::kCount; ++i) {
        const auto& weapon = m_motion.weapons()[i];
        if (!weapon.visible) {
            m_effects.stop(m_handles[i]);
            m_handles[i] = 0;
            continue;
        }
        if (m_handles[i] == 0) {
            EffectTrees::Setting setting;
            setting.persistent = true;
            m_handles[i] =
                m_effects.startSet(*m_device, m_archive, kNames[i], weapon.position, setting);
        }
        WorldCamera facing;
        facing.yaw = std::atan2(weapon.velocity.x, weapon.velocity.z);
        facing.pitch =
            std::atan2(-weapon.velocity.y, glm::length(Vec2{weapon.velocity.x, weapon.velocity.z}));
        Mat4 placement{1.0f};
        placement[0] = Vec4{facing.right(), 0};
        placement[1] = Vec4{facing.up(), 0};
        placement[2] = Vec4{facing.forward(), 0};
        placement[3] = Vec4{weapon.position, 1};
        placement = glm::rotate(placement, weapon.angle, Vec3{1, 0, 0});
        m_effects.placeAt(m_handles[i], placement);
    }
    m_effects.update(static_cast<f32>(seconds));
}

void IdleScreen::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height,
                        f32 horizontalFov) const {
    if (!m_open) {
        return;
    }
    const WorldCamera camera;
    const auto frame = CameraFrame::of(camera);
    m_effects.draw(device, camera.clipTransform(horizontalFov, width, height, projection), {},
                   &frame);
}
} // namespace gdl::game
