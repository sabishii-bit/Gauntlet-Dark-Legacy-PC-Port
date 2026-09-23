#pragma once
#include <cmath>
#include <filesystem>
#include <string_view>

#include "engine/core/Types.h"

#include "game/enemies/BossDefinition.h"
#include "game/enemies/Combatant.h"
namespace gdl::test {
/** One fighter with owned assets for tests of move execution, without encounter staging. */
struct CombatantFixture {
    game::CombatantAssets assets;
    game::Combatant actor;
    RenderDevice* device = nullptr;
    std::filesystem::path root;
    const WorldCollision* collision = nullptr;
    game::EnemyScales scales;
    char realm = 'G';
    f32 textureFrames = 0;
    void open(RenderDevice& renderer, const std::filesystem::path& directory,
              const WorldCollision* floor, const game::EnemyScales& tuning, char letter) {
        actor.clear();
        assets.clear();
        textureFrames = 0;
        device = &renderer;
        root = directory;
        collision = floor;
        scales = tuning;
        realm = letter;
    }
    bool spawn(std::string_view name, const Vec3& position, f32 yaw) {
        return assets.load(*device, root, game::bossDefinition(name), realm) &&
               actor.spawn(assets, 0, position, yaw, collision, scales, realm);
    }
    void update(s32 ticks, f32 seconds, std::span<const game::EnemyView> players) {
        if (ticks > 0) {
            textureFrames += seconds * AnimationPlayer::kDefaultRate;
            const auto frames = static_cast<u32>(std::floor(textureFrames));
            textureFrames -= static_cast<f32>(frames);
            assets.textures.step(frames);
        }
        actor.update(ticks, seconds, players);
    }
};
} // namespace gdl::test
