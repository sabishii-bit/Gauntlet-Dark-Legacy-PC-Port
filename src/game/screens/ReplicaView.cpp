#include "game/screens/ReplicaView.h"

#include <algorithm>
#include <cmath>

#include "engine/render/AmbientOcclusion.h"
#include "engine/render/DepthOfField.h"
#include "engine/ui/Canvas.h"

#include "game/players/CursorAim.h"
#include "game/players/PlayerActor.h"

namespace gdl::game {
bool ReplicaView::begin(const MatchContext& context, const LevelWorld& world, Enemies& enemies,
                        ProjectileResources& projectiles, PickupResources& pickups,
                        FixtureResources& fixtures, FighterResources fighters) {
    if (!context.valid() || context.epoch <= m_context.epoch || !world.built() ||
        context.transition == MatchTransition::Resume) {
        return false;
    }
    // Clear old-scene pointers/figures even when a seat's grant stayed unchanged.
    clear();
    m_context = context;
    m_world = &world;
    m_enemies = &enemies;
    m_resources = &projectiles;
    m_pickupResources = &pickups;
    m_fixtureResources = &fixtures;
    m_fighterResources = std::move(fighters);
    m_geometry = world.scene();
    return m_actors.begin(context.epoch) && m_projectiles.begin(context.epoch) &&
           m_pickups.begin(context.epoch) && m_fixtures.begin(context.epoch) &&
           m_fighters.begin(context.epoch);
}
bool ReplicaView::resume(const MatchContext& context) {
    if (m_world == nullptr || !context.valid() || context.epoch <= m_context.epoch ||
        context.transition != MatchTransition::Resume || context.scene != m_context.scene ||
        context.owners != m_context.owners || context.grants != m_context.grants ||
        context.tickRate != m_context.tickRate) {
        return false;
    }
    m_context = context;
    m_resuming = true;
    return true;
}
void ReplicaView::clear() {
    m_presentedClip.reset();
    m_presentedPlayers.fill(std::nullopt);
    m_context = {};
    m_world = nullptr;
    m_enemies = nullptr;
    m_resources = nullptr;
    m_pickupResources = nullptr;
    m_fixtureResources = nullptr;
    m_actors.clear();
    m_playerHeights.fill(PlayerActor::kDefaultHeight);
    m_projectiles.clear();
    m_pickups.clear();
    m_fixtures.clear();
    m_fighters.clear();
    m_fighterResources = {};
    m_geometry.clear();
    m_hud.clear();
    m_shown.reset();
    m_resuming = false;
}
bool ReplicaView::setPlayer(u8 seat, std::unique_ptr<PlayerFigure> figure, f32 scale,
                            const ClassStats* stats) {
    const bool accepted =
        m_world != nullptr && !m_shown && seat < m_context.owners.size() &&
        m_context.owners[seat] != 0 &&
        m_actors.setPlayer(seat, m_context.grants[seat], std::move(figure), scale);
    if (accepted) {
        m_playerHeights[seat] =
            stats != nullptr && stats->height > 0 ? stats->height : PlayerActor::kDefaultHeight;
    }
    return accepted;
}
bool ReplicaView::bindCompanions(RenderDevice& device, ItemArchive& powerups,
                                 ItemArchive* weapons) {
    return m_world != nullptr && !m_shown && m_actors.bindCompanions(device, powerups, weapons);
}
void ReplicaView::loadPortalSkin(RenderDevice& device, TextureSet& weapons) {
    m_actors.loadPortalSkin(device, weapons);
}
bool ReplicaView::loadHud(RenderDevice& device, const std::filesystem::path& root,
                          const StringTable* strings, const HudResources& resources) {
    if (m_world == nullptr || m_shown) {
        return false;
    }
    auto trusted = resources;
    trusted.levelTitle = m_world->level() != nullptr ? m_world->level()->title : std::string_view{};
    return m_hud.load(device, root, strings, trusted);
}
std::string_view ReplicaView::resourceRejection(const CombatSnapshot& snapshot) const {
    if (!m_actors.companionsReady(snapshot)) {
        return "companion resources";
    }
    if (snapshot.hud && !m_hud.accepts(*snapshot.hud)) {
        return "HUD resources";
    }
    for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
        const auto& motion = snapshot.motion.players[seat];
        if (motion.has_value() != (m_context.owners[seat] != 0) ||
            (motion && motion->grant != m_context.grants[seat])) {
            return "seat ownership";
        }
        const auto& state = snapshot.players[seat];
        if (state && state->life != ReplicaPlayerLife::InTower) {
            const auto* figure = m_actors.playerFigure(static_cast<u8>(seat));
            if (figure == nullptr || state->animation.action >= PlayerAnimator::kActionCount ||
                !ReplicaPose::accepts(figure->actionTree(), state->animation)) {
                return "player pose";
            }
        }
    }
    for (const auto& state : snapshot.enemies) {
        const auto kind = static_cast<s32>(state.kind);
        if (!m_enemies->kindLoaded(kind)) {
            return "enemy family";
        }
        if (kind == kItKind) {
            continue; // Logical multiplayer tagger; retail supplies no body mesh.
        }
        const bool asleep = state.life == ReplicaEnemyLife::Asleep;
        const auto* tree = m_enemies->appearanceTree(
            {kind, static_cast<s32>(state.tier), static_cast<s32>(state.variant), asleep});
        if (tree == nullptr ||
            ((kind != kDeathKind || !asleep) && !ReplicaPose::accepts(tree, state.animation))) {
            return "enemy pose";
        }
    }
    if (!std::ranges::all_of(snapshot.projectiles,
                             [&](const auto& shot) { return m_resources->accepts(shot); })) {
        return "projectile resources";
    }
    if (!std::ranges::all_of(snapshot.pickups,
                             [&](const auto& item) { return m_pickupResources->accepts(item); })) {
        return "pickup resources";
    }
    if (!std::ranges::all_of(snapshot.fixtures,
                             [&](const auto& item) { return m_fixtureResources->accepts(item); })) {
        return "fixture resources";
    }
    if (!std::ranges::all_of(snapshot.fighters,
                             [&](const auto& mesh) { return m_fighterResources.accepts(mesh); })) {
        return "fighter resources";
    }
    return {};
}
std::string_view ReplicaView::rejection(const CombatSnapshot& snapshot) const {
    if (m_world == nullptr || snapshot.motion.epoch != m_context.epoch) {
        return "stage epoch";
    }
    if (!snapshot.valid()) {
        return "snapshot bounds";
    }
    if (m_shown && !m_resuming && snapshot.motion.tick < m_shown->motion.tick) {
        return "stale snapshot";
    }
    if (!snapshot.geometry || !m_geometry.acceptsGeometry(*snapshot.geometry)) {
        return "stage geometry";
    }
    return resourceRejection(snapshot);
}
bool ReplicaView::show(const CombatSnapshot& snapshot) {
    if (!snapshot.geometry || !rejection(snapshot).empty()) {
        return false;
    }
    // All resource/epoch/sequence checks happen before any renderer is changed.
    if (m_resuming) {
        const auto epoch = m_context.epoch;
        if (!m_actors.begin(epoch) || !m_projectiles.begin(epoch) || !m_pickups.begin(epoch) ||
            !m_fixtures.begin(epoch) || !m_fighters.begin(epoch)) {
            return false;
        }
        m_resuming = false;
    }
    if (!m_actors.show(snapshot, *m_enemies) || !m_projectiles.show(snapshot, *m_resources) ||
        !m_pickups.show(snapshot, *m_pickupResources) ||
        !m_fixtures.show(snapshot, *m_fixtureResources) ||
        !m_fighters.show(snapshot, m_fighterResources)) {
        return false;
    }
    m_geometry.applyGeometry(*snapshot.geometry);
    m_shown = snapshot;
    return true;
}
std::optional<Rect> ReplicaView::viewport(f32 width, f32 height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        width > 65536 || height > 65536) {
        return std::nullopt;
    }
    return Rect{0, 0, width, height};
}
void ReplicaView::draw(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                       f32 frameHeight, f32 textureFrame, const Texture* hitFlash,
                       const Texture* frozen, const GameConfig& video) {
    Canvas canvas;
    canvas.begin(device, frameProjection);
    const auto area = m_shown ? viewport(frameWidth, frameHeight) : std::nullopt;
    if (!area || !m_shown || !std::isfinite(textureFrame) || textureFrame < 0) {
        m_presentedClip.reset();
        canvas.fillScreen(Color::black());
        canvas.end();
        return;
    }
    const auto& motion = m_shown->motion;
    const auto& camera = motion.camera;
    // Camera motion is shared gameplay state; projection and post-processing belong
    // to this display. A wider local window must not inherit the host's side masks.
    const Mat4 clip = camera.clipTransform(video.horizontalFovRadians(), frameWidth, frameHeight,
                                           frameProjection);
    m_presentedClip = clip;
    for (usize seat = 0; seat < motion.players.size(); ++seat) {
        const auto& player = motion.players[seat];
        m_presentedPlayers[seat] = player ? std::optional{player->position} : std::nullopt;
    }
    const auto frame = CameraFrame::of(camera);
    // Draw only host-controlled pickups; never tick the local world's item physics.
    m_geometry.drawOpaque(device, clip, frame);
    m_fixtures.draw(device, *m_fixtureResources, clip, m_world->lighting(), frame,
                    TreeModel::Pass::Opaque);
    m_pickups.draw(device, *m_pickupResources, clip, m_world->lighting(), frame,
                   TreeModel::Pass::Opaque);
    m_actors.draw(device, *m_enemies, clip, m_world->lighting(), frame, textureFrame, hitFlash,
                  TreeModel::Pass::DepthWriting);
    m_projectiles.draw(device, *m_resources, clip, m_world->fullLighting(), frame,
                       TreeModel::Pass::Opaque);
    m_fighters.draw(device, m_fighterResources, clip, m_world->lighting(), hitFlash, frozen,
                    TreeModel::Pass::DepthWriting);
    if (video.display.ambientOcclusion) {
        AmbientOcclusion occlusion;
        occlusion.clipToView = camera.view() * glm::inverse(clip);
        device.applyAmbientOcclusion(occlusion);
    }
    m_actors.drawShadows(device, clip, m_world->lighting(), camera.position);
    m_geometry.drawDeferred(device, clip, frame);
    m_fixtures.draw(device, *m_fixtureResources, clip, m_world->lighting(), frame,
                    TreeModel::Pass::Blended);
    m_pickups.draw(device, *m_pickupResources, clip, m_world->lighting(), frame,
                   TreeModel::Pass::Blended);
    m_actors.draw(device, *m_enemies, clip, m_world->lighting(), frame, textureFrame, hitFlash,
                  TreeModel::Pass::Effects);
    m_projectiles.draw(device, *m_resources, clip, m_world->fullLighting(), frame,
                       TreeModel::Pass::Blended);
    m_fighters.draw(device, m_fighterResources, clip, m_world->lighting(), hitFlash, frozen,
                    TreeModel::Pass::Effects);
    canvas.end();
    if (video.display.bloom) {
        device.applyBloom();
    }
    if (video.display.depthOfField) {
        DepthOfField blur;
        const Mat4 view = camera.view();
        blur.clipToView = view * glm::inverse(clip);
        for (usize seat = 0; seat < motion.players.size(); ++seat) {
            const auto& player = motion.players[seat];
            const auto& state = m_shown->players[seat];
            if (player && state && state->life == ReplicaPlayerLife::Standing) {
                const f32 distance = (view * Vec4{player->position, 1}).z;
                blur.focusEnd = std::max(blur.focusEnd, distance + m_playerHeights[seat] * 3);
            }
        }
        blur.transition = std::max(20.0f, blur.focusEnd);
        device.applyDepthOfField(blur);
    }
    if (m_shown->hud) {
        const Mat4 overlay =
            makeVirtualScreenTransform(frameProjection, 512, 384, frameWidth, frameHeight);
        canvas.begin(device, overlay);
        m_hud.draw(canvas, *m_shown->hud, glm::inverse(overlay) * clip);
        canvas.end();
    }
}
std::optional<Vec3> ReplicaView::cursorAim(u8 seat, Vec2 cursor) const {
    if (!m_shown || !m_presentedClip || seat >= m_presentedPlayers.size()) {
        return std::nullopt;
    }
    const auto& player = m_presentedPlayers[seat];
    return player ? cursorAimDirection(cursor, *m_presentedClip, *player) : std::nullopt;
}
} // namespace gdl::game
