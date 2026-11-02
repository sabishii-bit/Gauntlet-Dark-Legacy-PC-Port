#pragma once

#include <optional>
#include <string>
#include <vector>

#include "game/characters/ClassData.h"
#include "game/effects/EffectTrees.h"
#include "game/party/MoveStrikes.h"
#include "game/party/PlayerMissiles.h"

namespace gdl::game {
/** Callback-free preparation of the native DAMG/SFXX emission rows. A descriptor
 * is not a hit or permission to mutate the world: callers own resources, instance
 * identities, audio dispatch and authoritative contact/damage bookkeeping. */
struct TurboEmission {
    struct Context {
        s32 character = 0;
        Vec3 position{0};
        Vec3 facing{0, 0, 1};
        bool detached = false; ///< a combo impact supplied an explicit world position
        bool partnerAvailable = false;
    };
    enum class Actor : u8 { Caster, Partner };
    struct Attachment {
        Actor actor = Actor::Caster;
        bool bodyParent = false; ///< otherwise the first posed character node
        bool carrier = false;    ///< also carries the primary area strike
        Mat4 local{1};
    };
    struct Particle {
        ParticleDescriptor descriptor;
        std::string node;
        Vec3 offset{0};
        bool previousEffect = false;
        bool partnerFallback = false; ///< only when no previous effect was actually created
    };
    struct Effect {
        s32 row = -1;
        MoveEffect authored; ///< retain native flags and resource names for trusted resolution
        Vec3 position{0};
        EffectTrees::Setting setting;
        std::optional<Attachment> attachment;
        std::optional<Particle> particle;
        bool shake = false;
        bool primary = false;
        bool carriesFlight = false;
    };
    struct Strike {
        s32 row = -1;
        MoveStrike volume; ///< primary effect offset already added for native damage placement
        f32 effectSeconds = 0;
        /** Native initialized carrier, computed without dispatching its hits. Its
         * temporary id/owner are not an authoritative instance identity. */
        std::optional<MoveStrikes::Strike> carrier;
        std::vector<Effect> effects;
    };
    struct Volley {
        PlayerMissiles::WeaponThrow weapon;
        u32 soundFlags = 0; ///< native cue priority still sees fan/Super flags when spreads=false
        bool spreads = false;
    };

    /** Missing primary artwork yields zero, even if its SFXX record has a lifetime. */
    static f32 effectSeconds(const MoveEffect& effect, const TreeInfo* tree);
    static Particle particle(const MoveEffect& effect);
    static std::optional<Strike> prepare(const ClassStats& stats, s32 row, const Context& context,
                                         f32 meleeDamage, f32 effectSeconds);
    /** Native turbo volleys launch one ordinary weapon in the timeline's direction.
     * Inventory-derived flight/damage/element flags remain in the supplied descriptor. */
    static Volley volley(PlayerMissiles::WeaponThrow weapon, const Vec3& direction, u32 soundFlags);
};
} // namespace gdl::game
