#pragma once

#include <string>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"

#include "game/players/PowerupEffects.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {

/**
 * The glow an elemental weapon powerup puts in the hand (PlayerProcessPowerups, player.c
 * 5749): the `WEAP_HOLD_<colour>` effect of the costume colour's effects archive, kept going
 * while the element is worn and set at the weapon hand each frame with the class's offset
 * and scale for the character's tier; the thrown weapon carries the `WEAP_TW_<initial>`
 * effect of the same archive. It keeps only the effect's id, so the effect store must be
 * cleared with it or outlive it.
 */
class WeaponGlow {
public:
    static constexpr usize kTiers = 10; ///< one offset and scale a decade of levels

    /** The element an outfit puts in the hand: the weapon flags' own, unless the right
     * gauntlet, the super shot or the thunder hammer fills the hand instead. */
    static u32 elementOf(const PowerupEffects& worn);
    /** Whether a class throws the element's effect alone, its weapon unseen (the wizards
     * and sorceresses: PlayerStartMissile's char_type 2 and 6). */
    static bool throwsEffectAlone(s32 character);
    /** The tier of ten levels whose offset and scale a character of `level` takes. */
    static usize tierOf(s32 level);
    static std::string holdTree(u32 element);
    static std::string throwTree(u32 element);

    /** Keeps the glow of `element` (none puts it out) going in `effects` from `archive`,
     * placed at `hand` moved by `offset` and sized by `scale` (nought for none). */
    void update(RenderDevice& device, EffectTrees& effects, ItemArchive* archive, u32 element,
                const Mat4& hand, const Vec3& offset, const Vec3& scale);
    void clear(EffectTrees& effects);
    u32 element() const { return m_element; }
    u32 effect() const { return m_effect; }

private:
    u32 m_element = 0;
    u32 m_effect = 0;
};

} // namespace gdl::game
