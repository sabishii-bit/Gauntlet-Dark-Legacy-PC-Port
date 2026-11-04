#pragma once

#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/TreePose.h"

#include "game/party/PlayerFigure.h"
#include "game/players/PlayerAnimator.h"

namespace gdl::test {

/** What a figure's action state shows: its animator's clock, blend and pose, where its hand,
 * arm and root hang, its animation revision and whether its familiar has let fly. Two runs are
 * compared through it; it is never what a rewind restores. */
struct ActionObservation {
    game::PlayerAnimator::Checkpoint animation;
    std::optional<game::PlayerAnimator::TransitionPose> transition;
    std::optional<TreePose::Checkpoint> pose;
    std::optional<Mat4> hand;
    std::optional<Mat4> arm;
    Mat4 root{1};
    u64 revision = 0;
    bool familiarReleased = false;
    bool operator==(const ActionObservation&) const = default;
};

/** The action state `figure` shows now. */
inline ActionObservation observeAction(const game::PlayerFigure& figure) {
    return {figure.animator().checkpoint(),
            figure.animator().transitionPose(),
            figure.animator().pose().checkpoint(),
            figure.handAttachment(Mat4{1}),
            figure.armAttachment(Mat4{1}),
            figure.rootAttachment(Mat4{1}),
            figure.animationRevision(),
            figure.familiarReleased()};
}

} // namespace gdl::test
