#pragma once

#include <array>

#include "engine/core/Types.h"

namespace gdl::game {

/** What a player's buttons ask of it, the first that is held winning. Stored and digested by
 * value: add at the end, never reorder. */
enum class PlayerDeed : u8 {
    None,
    Attack,
    UsePotion,
    ThrowPotion,
    Die,
    Flinch,      ///< knocked back by a blow
    Spike,       ///< struck by spikes or a blade
    Reel,        ///< stunned, as by a fire trap
    TurboStrong, ///< the lesser turbo attack
    TurboFull,   ///< the greater
    Shove,
    Defend,       ///< one press: raise, block, then lower the guard
    StrongAttack, ///< the slow attack: a strong throw, with nothing in reach
    ShieldPotion, ///< a potion spent on a ring of its magic about the character
    FallBack,     ///< knocked off its feet from in front
    FallForward,  ///< or from behind
    // A legend item let fly at a boss, with the gesture its kind asks for: nothing else
    // leaves the hand meanwhile.
    HurlLegend,  ///< as a potion is used
    ThrowLegend, ///< as the strong throw
    ShootLegend, ///< the special shot
    Grabbed,
    Thrown,
    Webbed,
    Melee,
    MeleeLow,
    MeleeSlow,
    MeleeSlowLow,
    AutoMelee, ///< a moving body's close contact, not an attack-button press
    AutoMeleeLow,
    SuperShot,
    Hammer,
    Breathe,
    FireLeft,
    FireRight,
    Pick,       ///< the gesture of an item picked up
    Gag,        ///< retching at gas, Death's touch or food gone bad
    DeathGrab,  ///< a halo's hold on Death, drawing him off
    Whirled,    ///< swept up by a whirlwind
    Combo,      ///< takes hold of a partner for the class's two-player combo
    ComboHeld,  ///< in a partner's hands, playing the grabber's class's COMBO sequence
    ComboThrown ///< let go of by the partner: the warrior's pinball, the dwarf's charger
};

/** How many deeds there are. */
inline constexpr usize kPlayerDeedCount = static_cast<usize>(PlayerDeed::ComboThrown) + 1;

/** The sets of deeds party motion and the animator tell apart; several may be set. */
namespace deed_trait {
inline constexpr u32 kClose = 0x1;         ///< a close swing at what is within reach
inline constexpr u32 kLow = 0x2;           ///< at something short
inline constexpr u32 kStrong = 0x4;        ///< the strong attack's slow swing
inline constexpr u32 kAutomatic = 0x8;     ///< a moving body's unpressed contact swing
inline constexpr u32 kAttackButton = 0x10; ///< asked with the attack button down
inline constexpr u32 kStrongButton = 0x20; ///< asked with the strong attack button down
inline constexpr u32 kItem = 0x40;         ///< a worn powerup's attack
inline constexpr u32 kGauntlet = 0x80;     ///< a gauntlet's shot, from either hand
inline constexpr u32 kLegend = 0x100;      ///< a legend item's gesture
inline constexpr u32 kPotion = 0x200;      ///< a potion used, thrown or raised as a shield
/** Begins at once over an ordinary attack (DoPlayerAction's categories 11 and up). */
inline constexpr u32 kCutsAttack = 0x400;
inline constexpr u32 kCutsMelee = 0x800;  ///< may begin while a close attack plays
inline constexpr u32 kCutsGuard = 0x1000; ///< may begin while the guard is raised
inline constexpr u32 kHoldsBody = 0x2000; ///< while asked, the body stands, asked nothing else
inline constexpr u32 kAimed = 0x4000;     ///< a still body asking it turns to the assisted target
inline constexpr u32 kFells = 0x8000;     ///< a blow that puts the body off its feet
inline constexpr u32 kStrikes = 0x10000;  ///< a blow that cuts into whatever the body does
} // namespace deed_trait

/** What a deed is, beyond its value. */
struct DeedTraits {
    PlayerDeed deed = PlayerDeed::None;
    u32 traits = 0;      ///< deed_trait bits
    u8 reactionRank = 0; ///< a blow of higher rank takes a pending one's place; 0 for none
};

/** Every deed's traits, a row per deed in its order; read through deedTraitsOf. */
inline constexpr std::array<DeedTraits, kPlayerDeedCount> kDeedTraits{{
    {.deed = PlayerDeed::None},
    {.deed = PlayerDeed::Attack, .traits = deed_trait::kAttackButton | deed_trait::kAimed},
    {.deed = PlayerDeed::UsePotion, .traits = deed_trait::kPotion},
    {.deed = PlayerDeed::ThrowPotion, .traits = deed_trait::kPotion},
    {.deed = PlayerDeed::Die},
    {.deed = PlayerDeed::Flinch, .traits = deed_trait::kStrikes, .reactionRank = 3},
    {.deed = PlayerDeed::Spike, .traits = deed_trait::kStrikes, .reactionRank = 2},
    {.deed = PlayerDeed::Reel, .traits = deed_trait::kStrikes, .reactionRank = 1},
    {.deed = PlayerDeed::TurboStrong,
     .traits = deed_trait::kCutsAttack | deed_trait::kCutsMelee | deed_trait::kCutsGuard |
               deed_trait::kHoldsBody},
    {.deed = PlayerDeed::TurboFull,
     .traits = deed_trait::kCutsAttack | deed_trait::kCutsMelee | deed_trait::kCutsGuard |
               deed_trait::kHoldsBody},
    {.deed = PlayerDeed::Shove,
     .traits = deed_trait::kCutsMelee | deed_trait::kCutsGuard | deed_trait::kHoldsBody},
    {.deed = PlayerDeed::Defend},
    {.deed = PlayerDeed::StrongAttack,
     .traits = deed_trait::kStrongButton | deed_trait::kHoldsBody | deed_trait::kAimed},
    {.deed = PlayerDeed::ShieldPotion, .traits = deed_trait::kPotion},
    {.deed = PlayerDeed::FallBack,
     .traits = deed_trait::kFells | deed_trait::kStrikes,
     .reactionRank = 4},
    {.deed = PlayerDeed::FallForward,
     .traits = deed_trait::kFells | deed_trait::kStrikes,
     .reactionRank = 4},
    {.deed = PlayerDeed::HurlLegend,
     .traits = deed_trait::kLegend | deed_trait::kCutsGuard | deed_trait::kHoldsBody},
    {.deed = PlayerDeed::ThrowLegend, .traits = deed_trait::kLegend | deed_trait::kHoldsBody},
    {.deed = PlayerDeed::ShootLegend,
     .traits = deed_trait::kLegend | deed_trait::kCutsGuard | deed_trait::kHoldsBody},
    {.deed = PlayerDeed::Grabbed},
    {.deed = PlayerDeed::Thrown},
    {.deed = PlayerDeed::Webbed, .traits = deed_trait::kStrikes, .reactionRank = 1},
    {.deed = PlayerDeed::Melee,
     .traits = deed_trait::kClose | deed_trait::kAttackButton | deed_trait::kAimed},
    {.deed = PlayerDeed::MeleeLow,
     .traits =
         deed_trait::kClose | deed_trait::kLow | deed_trait::kAttackButton | deed_trait::kAimed},
    {.deed = PlayerDeed::MeleeSlow,
     .traits =
         deed_trait::kClose | deed_trait::kStrong | deed_trait::kStrongButton | deed_trait::kAimed},
    {.deed = PlayerDeed::MeleeSlowLow,
     .traits = deed_trait::kClose | deed_trait::kStrong | deed_trait::kLow |
               deed_trait::kStrongButton | deed_trait::kAimed},
    {.deed = PlayerDeed::AutoMelee, .traits = deed_trait::kClose | deed_trait::kAutomatic},
    {.deed = PlayerDeed::AutoMeleeLow,
     .traits = deed_trait::kClose | deed_trait::kAutomatic | deed_trait::kLow},
    {.deed = PlayerDeed::SuperShot,
     .traits = deed_trait::kItem | deed_trait::kCutsAttack | deed_trait::kCutsGuard},
    {.deed = PlayerDeed::Hammer,
     .traits =
         deed_trait::kItem | deed_trait::kCutsAttack | deed_trait::kCutsGuard | deed_trait::kAimed},
    {.deed = PlayerDeed::Breathe,
     .traits =
         deed_trait::kItem | deed_trait::kCutsAttack | deed_trait::kCutsGuard | deed_trait::kAimed},
    {.deed = PlayerDeed::FireLeft,
     .traits = deed_trait::kItem | deed_trait::kGauntlet | deed_trait::kAimed},
    {.deed = PlayerDeed::FireRight,
     .traits = deed_trait::kItem | deed_trait::kGauntlet | deed_trait::kAimed},
    {.deed = PlayerDeed::Pick},
    {.deed = PlayerDeed::Gag},
    {.deed = PlayerDeed::DeathGrab},
    {.deed = PlayerDeed::Whirled,
     .traits = deed_trait::kFells | deed_trait::kStrikes,
     .reactionRank = 5},
    {.deed = PlayerDeed::Combo,
     .traits = deed_trait::kCutsAttack | deed_trait::kCutsMelee | deed_trait::kCutsGuard},
    {.deed = PlayerDeed::ComboHeld},
    {.deed = PlayerDeed::ComboThrown},
}};

static_assert(
    [] {
        for (usize i = 0; i < kDeedTraits.size(); ++i) {
            if (kDeedTraits[i].deed != static_cast<PlayerDeed>(i)) {
                return false;
            }
        }
        return true;
    }(),
    "each deed's traits sit at its value's row");

/** The traits of `deed`, or null outside the deeds. */
[[nodiscard]] constexpr const DeedTraits* deedTraitsOf(PlayerDeed deed) {
    const auto row = static_cast<usize>(deed);
    return row < kPlayerDeedCount ? &kDeedTraits[row] : nullptr;
}

/** Whether `deed` has any of the deed_trait bits in `traits`; none outside the deeds. */
[[nodiscard]] constexpr bool deedHas(PlayerDeed deed, u32 traits) {
    const DeedTraits* row = deedTraitsOf(deed);
    return row != nullptr && (row->traits & traits) != 0;
}

/** How a blow's reaction ranks against one already pending, for PlayerImpact::combine; 0 for
 * a deed that is no blow and outside the deeds. */
[[nodiscard]] constexpr u8 reactionRank(PlayerDeed deed) {
    const DeedTraits* row = deedTraitsOf(deed);
    return row != nullptr ? row->reactionRank : u8{0};
}

/** Which close swing a deed asks for: pressed quick, pressed slow, or unpressed (always
 * quick). */
enum class CloseSwing : u8 { Quick, Slow, Automatic };

/** What a close swing is aimed at: something standing tall, or something low. */
enum class CloseHeight : u8 { High, Low };

/** The close deed of a swing at a height. */
[[nodiscard]] constexpr PlayerDeed closeDeedOf(CloseSwing swing, CloseHeight height) {
    const bool low = height == CloseHeight::Low;
    switch (swing) {
    case CloseSwing::Quick: return low ? PlayerDeed::MeleeLow : PlayerDeed::Melee;
    case CloseSwing::Slow: return low ? PlayerDeed::MeleeSlowLow : PlayerDeed::MeleeSlow;
    case CloseSwing::Automatic: return low ? PlayerDeed::AutoMeleeLow : PlayerDeed::AutoMelee;
    }
    return PlayerDeed::None;
}

} // namespace gdl::game
