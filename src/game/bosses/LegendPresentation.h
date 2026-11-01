#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/audio/SceneSoundEvents.h"
#include "game/bosses/LegendItems.h"
#include "game/bosses/LegendProfile.h"
#include "game/effects/EffectTrees.h"

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
        const LegendProfile* profile = nullptr; ///< borrowed immutable native presentation
    };
    /** Synchronous audio port: the caller selects banks, the presentation owns its loop. */
    struct Audio {
        std::function<SoundHandle(std::string_view)> play;
        std::function<void(SoundHandle)> stop;
        /** Resource availability, independent of whether the mixer made a voice. */
        std::function<bool(std::string_view)> available;
        /** An engaged result means recorded (even a silent/missing cue). A loop
         * reserves its native start token; gathering presents its final desire. */
        std::function<std::optional<SoundCueToken>(std::string_view, bool, s32, s32, u32)> record;
        std::function<void(std::string_view, s32, s32, SoundCueToken)> desire;
        /** Optional imported phase route. True suppresses ordinary named cues. */
        std::function<bool(LegendSound)> mapped;
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
        std::optional<Mat4> holdTransform = std::nullopt;
    };
    struct Target {
        Vec3 position{0.0f};
        f32 height = 0.0f;
        std::optional<Mat4> root = std::nullopt;
        /** Swept contact with the animated body, not just the homing anchor.
         * Called synchronously during update; never retained by the presentation. */
        std::function<bool(const Vec3&, const Vec3&, f32)> touches = nullptr;
        std::optional<Mat4> attachment = std::nullopt; ///< exact animated impact parent
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

    /** Shows the carried item before the boss rises, without starting its charge or sound. */
    void carry(s32 player, s32 realm, s32 kind, const std::optional<Bearer>& bearer);
    void show(LegendCue cue, s32 player, s32 realm, s32 kind, const std::optional<Bearer>& bearer);
    Update update(f32 seconds, const std::optional<Bearer>& bearer,
                  const std::optional<Target>& target);
    /** Stops only this presentation's effects and loop, leaving unrelated effects alone. */
    void clear();
    /** End this item within the current stage, retaining the local rewind binding. */
    void reset();
    /** Gather the still-owned flight sound once after replay's native step. */
    void gatherAudio() const;
    /** Presentation-boundary attachment guard; never used to decide gameplay. */
    bool hasLegacyAudio() const { return m_loop != kNoSound; }
    s32 player() const { return m_sim.player; }
    s32 kind() const { return m_sim.kind; }
    /** The player whose ordinary hand weapon is replaced, not an overhead relic's bearer. */
    s32 occupiedHand() const;
    /** Borrowed ice skin for the draw call, not part of the boss's gameplay state. */
    const Texture* frozenTexture() const { return m_sim.frozenTexture; }

private:
    LegendShow::Flight flight() const;
    std::string_view heldTree() const;
    std::string_view projectileTree() const;
    std::string_view burstTree() const;
    void brandish(const Bearer& bearer);
    void hold(const Bearer& bearer);
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
    struct Simulation {
        std::vector<u32> ownedEffects;
        s32 player = -1;
        s32 kind = -1;
        char realm = 'A';
        u32 held = 0;
        u32 flying = 0;
        u32 attached = 0;
        bool gestureOwed = false;
        bool released = false;
        bool brandished = false;
        f32 flightLeft = 0.0f;
        Vec3 flightPosition{0}; ///< Plague's SfxSetHitTarget flight tracks the animated eye
        const Texture* frozenTexture = nullptr;
        SoundCueToken flightCue;
        std::string flightSound;
    };

public:
    class LocalSnapshot {
        friend class LegendPresentation;
        Simulation m_state;
        const LegendPresentation* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    static void appendState(NativeStateDigest& digest, const Simulation& state);
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    SoundHandle m_loop = kNoSound;
};

} // namespace gdl::game
