#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/math/PortableMath.h"

#include "game/console/NoclipMotion.h"
#include "game/rollback/SharedConsoleWords.h"
#include "game/scenes/ConsoleStage.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SharedConsoleVerbs.h"

namespace gdl::game {

std::string PlayScene::applyConsoleAction(const ConsoleAction& action, ConsolePlay play) {
    if (!(play == ConsolePlay::Shared ? action.valid() : action.admitted()) || !m_open ||
        m_world == nullptr || m_device == nullptr) {
        return "No active scene for that console command.";
    }
    std::vector<std::string> words = splitConsoleWords(action.line);
    for (char& letter : words.front()) {
        if (letter >= 'A' && letter <= 'Z') {
            letter = static_cast<char>(letter - 'A' + 'a');
        }
    }
    const auto arguments = std::span<const std::string>{words}.subspan(1);
    const s32 player = action.seat;
    if (liveSave(player) == nullptr) {
        return "That player is no longer in the party.";
    }
    if (m_sim.consoleTravel || m_sim.leaving) {
        return "The party is already on its way.";
    }
    const SharedConsoleVerb* verb = sharedConsoleVerb(words.front());
    if (verb == nullptr) {
        return "Unknown shared console command.";
    }
    ConsoleStage stage(*this, play);
    return verb->apply(stage, player, arguments);
}

void PlayScene::advanceConsoleFlight(f64 deltaSeconds, const Inputs& inputs) {
    if (!m_open || !std::isfinite(deltaSeconds) || deltaSeconds <= 0) {
        return;
    }
    for (s32 player = 0; player < kPlayerCount; ++player) {
        if (!cheats(player).noclip) {
            continue;
        }
        Vec3 wish = inputs[static_cast<usize>(player)].consoleFlight;
        if (!consolePointInRange(wish)) {
            continue;
        }
        const f32 length = glm::length(wish);
        if (length == 0) {
            continue;
        }
        if (length > 1) {
            wish /= length;
        }
        // World-space, quantized local intent crosses the input protocol, never
        // a local render delta, camera transform or already-integrated position.
        const Vec3 offset = wish * NoclipMotion::kSpeed * static_cast<f32>(deltaSeconds);
        const std::optional<f32> yaw = wish.x * wish.x + wish.z * wish.z > 1.0e-6f
                                           ? std::optional{portable::atan2(wish.x, wish.z)}
                                           : std::nullopt;
        noclipMove(player, offset, yaw);
    }
}

bool PlayScene::pressSwitch(usize index) {
    return m_open && m_world != nullptr && m_world->pressTrigger(index);
}

bool PlayScene::unlockGate(usize index) {
    return m_open && m_fixtures.unlockGate(index);
}

bool PlayScene::lockGate(usize index) {
    return m_open && m_fixtures.lockGate(index);
}

bool PlayScene::setChestLocked(usize index, bool locked) {
    return m_open && m_fixtures.setChestLocked(index, locked);
}

ScenePicking::Scene PlayScene::pickingScene() {
    return {m_world, m_opponents, m_fixtures, m_players, m_spawned};
}

std::optional<SceneSelection> PlayScene::pick(Vec2 cursor) {
    if (!m_open || m_world == nullptr || !m_presentedClip.has_value()) {
        return std::nullopt;
    }
    return ScenePicking::pick(pickingScene(), cursor, *m_presentedClip, m_presentedView,
                              m_presentedWorldAlpha);
}

void PlayScene::markSelection(const SelectionHighlight* highlight) {
    m_picking.markSelection(pickingScene(), highlight);
}

bool PlayScene::noclipMove(s32 player, const Vec3& offset, std::optional<f32> yaw) {
    for (PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() != player) {
            continue;
        }
        if (runtime.departed || runtime.life != PlayerLife::Standing) {
            return false;
        }
        runtime.actor.place(runtime.actor.position() + offset);
        if (yaw.has_value()) {
            runtime.actor.turnTo(*yaw);
        }
        runtime.fixtureSpot.reset();
        return true;
    }
    return false;
}

} // namespace gdl::game
