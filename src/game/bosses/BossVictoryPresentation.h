#pragma once

#include <memory>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"

#include "game/bosses/BossVictory.h"
#include "game/camera/BossCamera.h"

namespace gdl::game {

/** Stages the victory visit, wizard animation and typed captions. Gameplay rewards,
 * audio playback and travel remain with the caller. Clear before releasing the item archive. */
class BossVictoryPresentation {
public:
    BossVictoryPresentation() = default;
    BossVictoryPresentation(const BossVictoryPresentation&) = delete;
    BossVictoryPresentation& operator=(const BossVictoryPresentation&) = delete;
    BossVictoryPresentation(BossVictoryPresentation&&) = delete;
    BossVictoryPresentation& operator=(BossVictoryPresentation&&) = delete;
    ~BossVictoryPresentation() = default;

    struct Update {
        std::vector<VictoryVoice> voices;
        bool sparkle = false; ///< one request per visit, even if the caller lacks effects
    };

    /** Begins the visit for a boss placed at `mark`, the level's boss locator. */
    void begin(s32 kind, char realm, u16 runesInRealm, u16 runesFound, bool goldLeft,
               const Vec3& mark);
    /** Borrows the wizard tree and resources. */
    void bindWizard(RenderDevice& device, ItemArchive& items);
    void clear();
    /** Start a scene tick even when another presentation holds the victory visit. */
    void capturePresentation() {
        m_sim.presentationAdvanced = false;
        m_textures.advance(0);
    }
    /** `party` is where the players standing or dying stand, in their slots' order: the wizard
     * comes among them in the update he begins to appear in. */
    Update update(s32 ticks, f32 seconds, bool goldLeft, const MessageTable& strings,
                  bool voicePlaying = false, std::span<const Vec3> party = {});
    void drawWizard(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                    const CameraFrame* camera = nullptr, f32 frameBlend = -1.0f) const;
    void drawCaption(Canvas& canvas, const TextPainter& text, const MessageTable& strings,
                     f32 width, f32 height) const;

    const BossVictory& state() const { return m_sim.visit; }
    BossCameraSubject wizardSubject() const;

private:
    struct Simulation {
        BossVictory visit;
        const TreeInfo* tree = nullptr;
        AnimationPlayer player;
        TreePose pose;
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
        bool presentationAdvanced = false;
        Vec3 mark{0.0f}; ///< the boss's locator, which the wizard comes between with the party
        Vec3 position{0.0f};
        f32 yaw = 0.0f;
        bool sparkled = false;
    };

public:
    class LocalSnapshot {
        friend class BossVictoryPresentation;
        Simulation m_state;
        TextureAnimator::Clock m_textureClock;
        const BossVictoryPresentation* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    void appendState(NativeStateDigest& digest, const Simulation& state,
                     const TextureAnimator::Clock& clock) const;
    void reset();
    void place(std::span<const Vec3> party);
    RenderDevice* m_boundDevice = nullptr;
    ItemArchive* m_boundItems = nullptr;
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    mutable TreeModel m_model;
    TextureAnimator m_textures;
};

} // namespace gdl::game
