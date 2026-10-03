#pragma once
#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

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
        std::string_view wallHitSound; ///< resolved from the level's audio record
        bool tower = false;
        bool bossEncounter = false;
        ItemArchive* powerups = nullptr;              ///< borrowed potion-cast feedback archive
        const MultiplayerMode* multiplayer = nullptr; ///< sampled when a weapon is launched
    };
    PlayerArsenal() = default;
    ~PlayerArsenal() = default;
    PlayerArsenal(const PlayerArsenal&) = delete;
    PlayerArsenal& operator=(const PlayerArsenal&) = delete;
    PlayerArsenal(PlayerArsenal&&) = delete;
    PlayerArsenal& operator=(PlayerArsenal&&) = delete;
    void bind(const Resources& resources, std::span<TextureSet* const> textureLenders = {});
    void clear();
    void launchWeapon(const PlayerActor& actor, PlayerFigure* body, const Vec3& direction,
                      f32 scale, bool spreads, std::optional<Vec3> target = std::nullopt);
    void launchSuperShot(PlayerActor& actor, PlayerFigure* body,
                         std::optional<Vec3> target = std::nullopt);
    void launchGauntlet(const PlayerActor& actor, PlayerFigure* body, bool left);
    void launchFamiliar(const PlayerActor& actor, PlayerFigure* body,
                        std::optional<Vec3> target = std::nullopt);
    std::optional<MissileImpact> usePotion(PlayerActor& actor);
    /** Unspecified magic advances one shared colour cycle for every caster and floor bottle. */
    s32 resolvePotionKind(s32 kind);
    void throwPotion(PlayerActor& actor, s32 heldTicks = 0);
    /** Keep short-lived cast effects attached without retaining a player address. */
    void followCaster(const PlayerActor& actor);
    void burstPotion(s32 kind, const Vec3& position, f32 power, bool castSound = true);
    /** Present a collision once, without applying target damage or expiry effects. */
    void presentImpact(const MissileImpact& impact, f32 playerDistance = 0);
    f32 magicPowerOf(const PlayerActor& actor) const;
    /** The power a potion of `kind` goes off with from `actor`: their magic's, a tenth more
     * for a potion of their own colour. */
    f32 potionPowerOf(const PlayerActor& actor, s32 kind) const;
    static constexpr f32 kPotionDamage = 40.0f; ///< start_magic's, before the colour bonus
    PlayerMissiles& missiles() { return m_missiles; }
    const PlayerMissiles& missiles() const { return m_missiles; }

private:
    void loadPotionModels();
    void healingCast(const PlayerActor& actor, f32 power);
    struct CastEffect {
        s32 owner;
        u32 effect;
    };
    std::vector<CastEffect> m_castEffects;
    std::optional<Resources> m_resources;
    std::array<TreeModel, 5> m_potionModels;
    TreeModel m_superShot;
    TreeModel m_phoenixShot;
    std::array<TreeModel, 2> m_gauntlets;
    PlayerMissiles m_missiles;
    s32 m_nextPotionKind = 1;
};
} // namespace gdl::game
