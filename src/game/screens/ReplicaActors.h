#pragma once

#include <map>

#include "game/enemies/Enemies.h"
#include "game/netplay/ReplicaPose.h"
#include "game/screens/PortalDeparture.h"
#include "game/screens/ReplicaCompanions.h"
#include "game/world/PlayerFigure.h"

namespace gdl::game {
/** Client base-actor presentation. Player assets are supplied by the trusted
 * roster/load step; enemy assets must already be loaded in the resource owner.
 * No packets cause file loading, spawning, AI, input, saves or damage callbacks.
 * The resource owner must outlive this view. This does not yet draw replicated
 * equipment, death skins, projectiles or transient VFX. */
class ReplicaActors {
public:
    bool begin(u64 epoch);
    void clear();
    bool setPlayer(u8 seat, u32 grant, std::unique_ptr<PlayerFigure> figure, f32 scale = 1);
    bool bindCompanions(RenderDevice& device, ItemArchive& powerups, ItemArchive* weapons);
    void loadPortalSkin(RenderDevice& device, TextureSet& weapons);
    bool companionsReady(const CombatSnapshot& snapshot) const;
    bool show(const CombatSnapshot& snapshot, Enemies& resources);
    void draw(RenderDevice& device, Enemies& resources, const Mat4& clip,
              const WorldLighting& lighting, const CameraFrame& camera, f32 textureFrame,
              const Texture* hitFlash, TreeModel::Pass pass = TreeModel::Pass::All) const;
    usize enemyCount() const { return m_enemies.size(); }
    void drawShadows(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                     const Vec3& eye) const;
    usize visiblePlayers() const;
    const PlayerFigure* playerFigure(u8 seat) const;

private:
    struct Player {
        u32 grant = 0;
        u32 continuity = 0;
        f32 scale = 1;
        bool shown = false;
        bool hitFlash = false;
        std::optional<f32> portalPhase;
        std::optional<PlayerShadowState> shadow;
        Mat4 placement{1};
        PlayerAnimator::Action action = PlayerAnimator::Action::Ready;
        std::unique_ptr<PlayerFigure> figure;
        ReplicaPose pose;
        CompanionResources resources;
        std::array<std::optional<CompanionState>, 2> companions;
    };
    struct Enemy {
        const TreeInfo* tree = nullptr;
        Enemies::Appearance appearance;
        CombatAnimation animation;
        bool shown = false;
        bool hitFlash = false;
        Mat4 placement{1};
        ReplicaPose pose;
    };
    u64 m_epoch = 0;
    std::optional<u64> m_tick;
    std::array<Player, InputCommand::kSeats> m_players;
    PortalDeparture m_portalSkin;
    std::map<u64, Enemy> m_enemies;
};
} // namespace gdl::game
