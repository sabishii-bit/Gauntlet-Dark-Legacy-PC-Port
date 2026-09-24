#pragma once

#include <array>
#include <bitset>
#include <filesystem>
#include <random>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/platform/Input.h"

#include "game/world/EffectTrees.h"

namespace gdl::game {

/** Tracks inactivity across all controllers without mistaking analogue noise for input. */
class IdleWatch {
public:
    static constexpr f64 kWaitSeconds = 600.0;
    bool update(f64 seconds, const Input& input, bool eligible);
    void reset();
    bool active() const { return m_active; }
    bool consumingInput() const { return m_releasing; }

private:
    static constexpr usize kKeys = static_cast<usize>(Key::Count);
    static constexpr usize kButtons = static_cast<usize>(PadButton::Count);
    static constexpr usize kAxes = static_cast<usize>(PadAxis::Count);
    static constexpr usize kPadBits = kButtons + 2 * kAxes;
    using Buttons = std::bitset<kKeys + Input::kMaxPads * kPadBits>;
    static Buttons snapshot(const Input& input);
    Buttons m_previous;
    f64 m_seconds = 0.0;
    bool m_active = false;
    bool m_releasing = false;
};

/** Camera-space weapon flight, independent of the scene hidden by the screensaver. */
class SaverMotion {
public:
    static constexpr usize kCount = 4;
    struct Weapon {
        Vec3 position{0.0f};
        Vec3 velocity{0.0f};
        f32 angle = 0.0f;
        s32 elapsed = 0;
        s32 delay = 0;
        s32 resetAt = 0;
        s32 collision = 0;
        bool visible = false;
    };
    void start();
    void update(f64 seconds, f32 horizontalFov, f32 aspect);
    const std::array<Weapon, kCount>& weapons() const { return m_weapons; }

private:
    void step(f32 horizontalFov, f32 aspect);
    f32 random(f32 range);
    std::array<Weapon, kCount> m_weapons;
    std::minstd_rand m_random{1};
    f64 m_remainder = 0.0;
};

/** The animated, flaming weapon screensaver; the application suspends its underlying scene. */
class IdleScreen {
public:
    bool open(RenderDevice& device, const std::filesystem::path& root);
    void close();
    bool isOpen() const { return m_open; }
    void update(f64 seconds, f32 horizontalFov, f32 aspect);
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height,
                f32 horizontalFov) const;
    const EffectTrees& effects() const { return m_effects; }

private:
    ItemArchive m_archive;
    EffectTrees m_effects;
    SaverMotion m_motion;
    std::array<u32, SaverMotion::kCount> m_handles{};
    RenderDevice* m_device = nullptr;
    bool m_open = false;
};

} // namespace gdl::game
