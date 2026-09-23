#pragma once
#include <array>
#include <optional>

#include "game/players/PlayerActor.h"
#include "game/world/EffectTrees.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/PlayerFigure.h"
#include "game/world/PlayerMissiles.h"
namespace gdl::game {
/** Owns player projectiles and thrown-potion models. Bound services are borrowed;
 * clear before releasing figures, effects, world collision or the weapons archive.
 * Models/missiles retain addresses inside this owner, so it must not move. */
class PlayerArsenal {
public:
    static constexpr f32 kBurstPerPower = 0.03125f;
    struct Resources {
        RenderDevice& device;
        const ClassDataSet& classes;
        ItemArchive& weapons;
        const WorldCollision& collision;
        EffectTrees& effects;
        LevelSoundscape& audio;
        SoundPlayer* sounds;
    };
    PlayerArsenal() = default;
    ~PlayerArsenal() = default;
    PlayerArsenal(const PlayerArsenal&) = delete;
    PlayerArsenal& operator=(const PlayerArsenal&) = delete;
    PlayerArsenal(PlayerArsenal&&) = delete;
    PlayerArsenal& operator=(PlayerArsenal&&) = delete;
    void bind(const Resources& resources);
    void clear();
    void launchWeapon(const PlayerActor& actor, PlayerFigure* body, const Vec3& direction,
                      f32 scale, bool spreads);
    void usePotion(PlayerActor& actor);
    void throwPotion(PlayerActor& actor);
    void burstPotion(s32 kind, const Vec3& position, f32 power);
    f32 magicPowerOf(const PlayerActor& actor) const;
    PlayerMissiles& missiles() { return m_missiles; }
    const PlayerMissiles& missiles() const { return m_missiles; }

private:
    void loadPotionModels();
    std::optional<Resources> m_resources;
    std::array<TreeModel, 5> m_potionModels;
    PlayerMissiles m_missiles;
};
} // namespace gdl::game
