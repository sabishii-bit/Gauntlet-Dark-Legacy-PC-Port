#pragma once

#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/MessageTable.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"

#include "game/screens/BossVictory.h"
#include "game/world/BossCamera.h"

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

    void begin(s32 kind, char realm, u16 runesInRealm, u16 runesFound, bool goldLeft);
    /** Borrows the wizard tree and resources; party contains only standing participants. */
    void bindWizard(RenderDevice& device, ItemArchive& items, const Vec3& boss,
                    std::span<const Vec3> party);
    void clear();
    Update update(s32 ticks, f32 seconds, bool goldLeft, const MessageTable& strings);
    void drawWizard(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void drawCaption(Canvas& canvas, const TextPainter& text, const MessageTable& strings,
                     f32 width, f32 height) const;

    const BossVictory& state() const { return m_visit; }
    BossCameraSubject wizardSubject() const;

private:
    BossVictory m_visit;
    const TreeInfo* m_tree = nullptr;
    TreeModel m_model;
    AnimationPlayer m_player;
    TreePose m_pose;
    Vec3 m_position{0.0f};
    f32 m_yaw = 0.0f;
    bool m_sparkled = false;
};

} // namespace gdl::game
