#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

struct LevelRef;

/** What the N64's level-name pictures are named by, before their exit's code. */
inline constexpr std::string_view kLegendsN64PlaquePrefix = "LNAME_";

/** The N64's picture naming the level an exit of code `code` leads to: LNAME_ and the code's
 * realm and level bytes in hex (FUN_e0058560, e00aae08), LNAME_0701 for the town's first. */
std::string legendsN64PlaqueArt(u32 code);

/** The text id of Dark Legacy's name for realm `realmId`, as its scrolls name the realms: one for
 * each of the eleven an exit of the tower may lead to, the castle 1 to the sky 11; none for any
 * other. */
std::optional<std::string> darkLegacyRealmNameText(s32 realmId);

/**
 * The plaque over a seat's N64 panel naming the exit the seat stands on, faded in and out as
 * FUN_e0058620 fades the N64's, one step a rebuild of the HUD. Made at transparency 0xdf as the
 * seat steps on, it clears by 0x20 a step to rest at 0x1f. When the seat steps off, or onto
 * another exit, it clouds over by 0x40 a step until past 0xbe, and then lets the exit go: the
 * next exit's plaque takes its place, or with none it clouds on by 0x20 a step until past 0xde
 * and goes. An exit without a picture is answered all the same, but shows no plaque.
 */
class LegendsN64Plaque {
public:
    /** The plaque as it shows. */
    struct Shown {
        u32 code = 0;        ///< the exit it was made for
        u8 transparency = 0; ///< the N64's, from 0 opaque to 255 clear
        /** Made for an exit of Dark Legacy's, which no N64 picture names: the port's own plaque,
         * writing the realm and the level the exit leads to in the N64's fonts. */
        bool darkLegacy = false;
        s32 realmId = -1;  ///< that level's realm
        std::string title; ///< and its title, in capitals as the plaque writes it
        bool operator==(const Shown&) const = default;
    };

    /** One step for a seat on the exit `wanted` names; none off every exit. A Dark Legacy exit
     * also names `destination`, where leaving by it takes the party, and counts as none when that
     * is the tower, which no text of Dark Legacy's names. `pictured` is whether there is a plaque
     * to show for it: an N64 code without its picture has none, as FUN_e0058560 makes none. */
    void step(std::optional<u32> wanted, const LevelRef* destination, bool pictured);
    /** Stands the plaque where a seat on `wanted` from long before would have it, at rest, or
     * gone off every exit; a rewind takes it there, as the steps it would have taken are gone. */
    void snap(std::optional<u32> wanted, const LevelRef* destination, bool pictured);
    /** The plaque as it shows this step; none while there is none. */
    const std::optional<Shown>& shown() const { return m_shown; }
    /** The exit the plaque answers to: it clears while its seat stands on it, and clouds over
     * once the seat leaves; none once it has let it go. */
    std::optional<u32> answering() const;

private:
    u32 m_code = 0;        ///< the exit answered to; 0 for none
    u8 m_transparency = 0; ///< the seat's byte, which the plaque shows while there is one
    std::optional<Shown> m_shown;
};

} // namespace gdl::game
