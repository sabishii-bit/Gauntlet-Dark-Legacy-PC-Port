#include "game/screens/ReplicaActors.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
namespace {
Mat4 placement(const Vec3& position, f32 yaw, f32 scale = 1) {
    return glm::scale(glm::rotate(glm::translate(Mat4{1}, position), yaw, Vec3{0, 1, 0}),
                      Vec3{scale});
}
} // namespace

bool ReplicaActors::begin(u64 epoch) {
    if (epoch == 0 || epoch <= m_epoch) {
        return false;
    }
    m_epoch = epoch;
    m_tick.reset();
    m_enemies.clear();
    for (auto& player : m_players) {
        player.shown = false;
        player.continuity = 0;
        player.pose.bind(player.figure ? player.figure->actionTree() : nullptr);
    }
    return true;
}
void ReplicaActors::clear() {
    m_epoch = 0;
    m_tick.reset();
    m_players = {};
    m_enemies.clear();
}
bool ReplicaActors::setPlayer(u8 seat, u32 grant, std::unique_ptr<PlayerFigure> figure, f32 scale) {
    if (seat >= m_players.size() || grant == 0 || !figure || !std::isfinite(scale) || scale <= 0 ||
        scale > 16) {
        return false;
    }
    auto& player = m_players[seat];
    player = {};
    player.grant = grant;
    player.scale = scale;
    player.pose.bind(figure->actionTree());
    player.resources.bindEarned(*figure);
    player.figure = std::move(figure);
    return true;
}
bool ReplicaActors::bindCompanions(RenderDevice& device, ItemArchive& powerups,
                                   ItemArchive* weapons) {
    if (m_tick) {
        return false;
    }
    std::array<CompanionResources, InputCommand::kSeats> ready;
    for (usize seat = 0; seat < m_players.size(); ++seat) {
        const auto& player = m_players[seat];
        if (player.figure) {
            ready[seat].bindEarned(*player.figure);
            if (!ready[seat].bindPowerups(device, powerups, weapons)) {
                return false;
            }
        }
    }
    for (usize seat = 0; seat < m_players.size(); ++seat) {
        m_players[seat].resources = std::move(ready[seat]);
    }
    return true;
}
bool ReplicaActors::companionsReady(const CombatSnapshot& snapshot) const {
    for (usize seat = 0; seat < m_players.size(); ++seat) {
        const auto& player = snapshot.players[seat];
        if (!player) {
            continue;
        }
        const auto& companions = player->companions;
        for (usize slot = 0; slot < companions.size(); ++slot) {
            if (companions[slot] && !m_players[seat].resources.accepts(slot, *companions[slot])) {
                return false;
            }
        }
    }
    return true;
}
bool ReplicaActors::show(const CombatSnapshot& snapshot, Enemies& resources) {
    if (m_epoch == 0 || snapshot.motion.epoch != m_epoch || !snapshot.valid() ||
        (m_tick && snapshot.motion.tick < *m_tick) || !companionsReady(snapshot)) {
        return false;
    }
    m_tick = snapshot.motion.tick;
    for (usize seat = 0; seat < m_players.size(); ++seat) {
        auto& player = m_players[seat];
        const auto& state = snapshot.players[seat];
        const auto& motion = snapshot.motion.players[seat];
        player.shown = false;
        player.companions = {};
        if (!state || !motion || !player.figure || player.grant != motion->grant ||
            state->life == ReplicaPlayerLife::InTower ||
            state->animation.action >= PlayerAnimator::kActionCount) {
            player.pose.bind(player.figure ? player.figure->actionTree() : nullptr);
            player.continuity = 0;
            continue;
        }
        if (player.continuity != motion->continuity) {
            player.pose.bind(player.figure->actionTree());
            player.continuity = motion->continuity;
        }
        player.shown = player.pose.show(state->animation);
        player.action = static_cast<PlayerAnimator::Action>(state->animation.action);
        player.hitFlash = state->hitFlash;
        player.placement = placement(motion->position, motion->yaw, player.scale);
        player.companions = state->companions;
    }
    std::erase_if(m_enemies, [&](const auto& row) {
        const auto found =
            std::ranges::lower_bound(snapshot.enemies, row.first, {}, &EnemyCombatState::instance);
        return found == snapshot.enemies.end() || found->instance != row.first;
    });
    for (const auto& state : snapshot.enemies) {
        auto& enemy = m_enemies[state.instance];
        enemy.appearance = {static_cast<s32>(state.kind), static_cast<s32>(state.tier),
                            static_cast<s32>(state.variant),
                            state.life == ReplicaEnemyLife::Asleep};
        const auto* tree = resources.appearanceTree(enemy.appearance);
        if (tree != enemy.tree) {
            enemy.tree = tree;
            enemy.pose.bind(tree);
        }
        const bool statue = state.kind == kDeathKind && enemy.appearance.asleep;
        enemy.shown = statue ? enemy.pose.rest() : enemy.pose.show(state.animation);
        enemy.animation = state.animation;
        enemy.hitFlash = state.hitFlash;
        enemy.placement = placement(state.position, state.yaw);
    }
    return true;
}
void ReplicaActors::draw(RenderDevice& device, Enemies& resources, const Mat4& clip,
                         const WorldLighting& lighting, const CameraFrame& camera, f32 textureFrame,
                         const Texture* hitFlash, TreeModel::Pass pass) const {
    if (!std::isfinite(textureFrame) || textureFrame < 0) {
        return;
    }
    for (const auto& player : m_players) {
        if (player.shown && player.figure) {
            player.figure->drawPose(device, clip, player.placement, lighting, player.pose.pose(),
                                    player.action, textureFrame,
                                    player.hitFlash ? hitFlash : nullptr, &camera, pass);
            for (usize slot = 0; slot < player.companions.size(); ++slot) {
                if (const auto& companion = player.companions[slot]) {
                    player.resources.draw(device, slot, *companion, clip, lighting, camera, pass);
                }
            }
        }
    }
    for (const auto& [instance, enemy] : m_enemies) {
        if (enemy.shown) {
            resources.drawPose(device, clip, enemy.placement, lighting, enemy.appearance,
                               enemy.pose.pose(), enemy.animation.sequence, enemy.animation.frame,
                               textureFrame, enemy.hitFlash ? hitFlash : nullptr, &camera, pass);
        }
    }
}
usize ReplicaActors::visiblePlayers() const {
    return static_cast<usize>(
        std::ranges::count_if(m_players, [](const auto& player) { return player.shown; }));
}
const PlayerFigure* ReplicaActors::playerFigure(u8 seat) const {
    return seat < m_players.size() ? m_players[seat].figure.get() : nullptr;
}
} // namespace gdl::game
