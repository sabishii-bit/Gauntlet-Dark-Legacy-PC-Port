#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/LegendItems.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {

/** The legend item's held effect, throw gesture, flight and sounds. Gameplay consumes
 * the returned impact; this component never damages a boss or changes a player. */
class LegendPresentation {
public:
    /** Borrowed archives/device must outlive the presentation, as must its effect store. */
    struct Assets {
        RenderDevice& device;
        ItemArchive& items;
        ItemArchive& weapons;
        TextureSet& sharedTextures;
    };
    /** Synchronous audio port: the caller selects banks, the presentation owns its loop. */
    struct Audio {
        std::function<SoundHandle(std::string_view)> play;
        std::function<void(SoundHandle)> stop;
    };
    /** Current pose supplied by the scene, independent of its actor/figure storage. */
    struct Bearer {
        s32 player = -1;
        s32 color = 0;
        Vec3 position{0.0f};
        Vec3 facing{0.0f, 0.0f, 1.0f};
        Vec3 holdPoint{0.0f};
        bool canGesture = false;
        bool casting = false;
        bool released = false;
    };
    struct Target {
        Vec3 position{0.0f};
        f32 height = 0.0f;
        std::optional<Mat4> root = std::nullopt;
    };
    struct Update {
        PlayerDeed gesture = PlayerDeed::None;
        bool landed = false;
    };

    LegendPresentation(EffectTrees& effects, Assets assets, Audio audio);
    ~LegendPresentation();
    LegendPresentation(const LegendPresentation&) = delete;
    LegendPresentation& operator=(const LegendPresentation&) = delete;
    LegendPresentation(LegendPresentation&&) = delete;
    LegendPresentation& operator=(LegendPresentation&&) = delete;

    void show(LegendCue cue, s32 player, s32 realm, s32 kind, const std::optional<Bearer>& bearer);
    Update update(f32 seconds, const std::optional<Bearer>& bearer,
                  const std::optional<Target>& target);
    /** Stops only this presentation's effects and loop, leaving unrelated effects alone. */
    void clear();
    s32 player() const { return m_player; }
    s32 kind() const { return m_kind; }
    /** Borrowed ice skin for the draw call, not part of the boss's gameplay state. */
    const Texture* frozenTexture() const { return m_frozenTexture; }

private:
    void brandish(const Bearer& bearer);
    void release(const Bearer& bearer, const std::optional<Target>& target);
    void land(const std::optional<Target>& target);
    void followTarget(const std::optional<Target>& target);
    void playSound(LegendShow::Sound sound, bool looping = false);
    void stopLoop();
    u32 start(ItemArchive& archive, std::string_view tree, const Vec3& position,
              const EffectTrees::Setting& setting);

    EffectTrees& m_effects;
    Assets m_assets;
    Audio m_audio;
    std::vector<u32> m_ownedEffects;
    s32 m_player = -1;
    s32 m_kind = -1;
    char m_realm = 'A';
    u32 m_held = 0;
    u32 m_flying = 0;
    u32 m_attached = 0;
    bool m_gestureOwed = false;
    bool m_released = false;
    f32 m_flightLeft = 0.0f;
    SoundHandle m_loop = kNoSound;
    const Texture* m_frozenTexture = nullptr;
};

} // namespace gdl::game
