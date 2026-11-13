#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/characters/CharacterSave.h"

namespace gdl::game {

/** The code Sumner's gateway row answers to among the topics of any tower's Sumner: clear of
 * the original's topic codes (39 to 42, the items of OPTMENU 0x8011F804) and of every native
 * resident's. */
inline constexpr s32 kGatewayTopic = 0x100;
/** The text id of that row, which opens his list of where the gateways lead. */
inline constexpr std::string_view kGatewayTopicText = "gateway.topic";
/** The text id of the campaign's own tower in that list. */
inline constexpr std::string_view kGatewayHomeText = "gateway.home";

/** A world past the campaign's own that an optional expansion leads to through Sumner's
 * gateways. */
struct GatewayWorld {
    u8 id = 0;           ///< which world: its expansion's own number for it, on every install
    std::string label;   ///< the text id of its name among Sumner's choices
    bool usable = false; ///< whether this installation holds everything its tower needs

    bool operator==(const GatewayWorld&) const = default;
};

/** The worlds Sumner's gateways lead to, as the expansions built into the game offer them.
 * Nothing is looked for until asked: never as the game starts, and never online, where an open
 * scene is given none. */
class TowerGateways {
public:
    virtual ~TowerGateways() = default;
    /** Every world offered, usable on this installation or not, in the order Sumner names them;
     * each ask looks on disk afresh. */
    virtual std::span<const GatewayWorld> worlds() = 0;
};

/** Where Sumner's gateways take a party: the campaign's own tower, or a world past them. */
struct GatewayDestination {
    std::optional<u8> world; ///< the world's id; none for the campaign's own tower

    bool operator==(const GatewayDestination&) const = default;
};

/** A place in Sumner's list of where his gateways lead. */
struct GatewayChoice {
    GatewayDestination destination;
    std::string label;    ///< the text id of its name
    bool enabled = false; ///< whether it can be chosen

    bool operator==(const GatewayChoice&) const = default;
};

/** Sumner's list of where his gateways lead, alike in every tower: the campaign's own tower,
 * then each world past them in the order offered. A world this installation cannot open, and
 * the place the party stands in (`here`), are greyed. */
std::vector<GatewayChoice> gatewayChoices(std::span<const GatewayWorld> worlds,
                                          GatewayDestination here);

/** Whether `save`'s character has beaten Garm, which wakes the gateways: its class holds his
 * piece, the battlefield's place in the tower's order (0x80124D14), which BossDeath gives every
 * player standing as he falls (PlayerGiveRune 0x800A2500). */
bool beatenGarm(const CharacterSave& save);

} // namespace gdl::game
