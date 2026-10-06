#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldLighting.h"

#include "game/players/PowerupEffects.h"
#include "game/screens/PlayerRuntime.h"
#include "game/screens/PortalDeparture.h"
#include "game/world/EffectTrees.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

/**
 * How the party is drawn in a level: each figure still there in its skin (the portal's, the
 * damage flash, or the chrome of invulnerability), with what the second arm bears (the left
 * gauntlet, else a shield armour), its headwear and, on who is it, the realm's sign; its
 * shadow on the floor under it; and in a dark level the lantern each standing player carries.
 * It keeps only the skins, borrowed from the powerup and weapon archives: clear it before
 * they are released.
 */
class PartyFigures {
public:
    static constexpr std::string_view kHitFlashSkin = "AAAWHITE";   ///< of POWERUPS
    static constexpr std::string_view kGoldSkin = "CHROMEGOLD";     ///< of WEAPONS
    static constexpr std::string_view kSilverSkin = "CHROMESILVER"; ///< of WEAPONS

    /** What the figures borrow of the scene for one frame. */
    struct Scene {
        LevelWorld& world;
        ItemArchive& weapons;             ///< the shields borne on the arm
        const PortalDeparture& departure; ///< the sinking and its skin
        f32 frameBlend = 1.0f;
        s32 occupiedHand = -1; ///< player id carrying a hand-mounted legend item
    };

    /** Finds the damage flash in `powerups` and the chrome skins in `weapons` (InitEffects). */
    void loadSkins(RenderDevice& device, ItemArchive& powerups, ItemArchive& weapons);
    void clear();
    /** Snapshot before a fixed simulation step; render interpolation never moves actors. */
    static void snapshot(std::span<PlayerRuntime> players);
    static Mat4 presentationBody(const PlayerRuntime& runtime, f32 frameBlend);
    static f32 presentationBlend(const PlayerRuntime& runtime, f32 frameBlend);
    const Texture* hitFlash() const { return m_hitFlash; }
    /** The skin a figure wears this frame, none for its own: the portal's while it sinks,
     * else the damage flash, else the chrome its invulnerability shows. */
    const Texture* skinOf(const PlayerRuntime& runtime, const PowerupEffects& worn,
                          const PortalDeparture& departure) const;

    /** The shield an armour bears on the arm, none without one: reflecting, fire, then
     * lightning, as PlayerProcessPowerups tries them. */
    static std::string_view shieldObjectOf(u32 armor);

    void draw(RenderDevice& device, std::span<const PlayerRuntime> players, const Scene& scene,
              const Mat4& clip, const CameraFrame& camera) const;
    /** Follows each member's head gem, bursting GETGEMORANGE about them as one appears
     * (StartGemFX, player.c 5633). */
    static void greetGems(RenderDevice& device, std::span<PlayerRuntime> players,
                          ItemArchive& powerups, EffectTrees& effects);
    /** Advance independently placed Mikey figures, including their arrival sparkles. */
    static void updateDecoys(RenderDevice& device, std::span<PlayerRuntime> players,
                             ItemArchive& powerups, f32 seconds, EffectTrees& effects);
    static void drawShadows(RenderDevice& device, std::span<const PlayerRuntime> players,
                            const Scene& scene, const Mat4& clip, const Vec3& eye);
    /** Adds the lanterns the standing carry when `level` is dark (player.c 2499). */
    static void addLanterns(std::vector<PointLight>& lights, std::span<const PlayerRuntime> players,
                            const LevelInfo* level);

private:
    /** Whether the figure is to be seen at all: there, still in the level, not yet gone
     * through the portal. */
    static bool shown(const PlayerRuntime& runtime, const Scene& scene);
    static f32 alphaOf(const PlayerRuntime& runtime, const PowerupEffects& worn);

    const Texture* m_hitFlash = nullptr;
    const Texture* m_goldSkin = nullptr;
    const Texture* m_silverSkin = nullptr;
};

} // namespace gdl::game
