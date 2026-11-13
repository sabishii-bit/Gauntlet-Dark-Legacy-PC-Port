#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCamera.h"

#include "game/characters/Party.h"
#include "game/hud/LevelMessages.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayerRuntime.h"
#include "game/tower/LevelBeaten.h"
#include "game/tower/SumnerFigure.h"
#include "game/tower/TowerGateways.h"

namespace gdl::game {

/**
 * The spot before Sumner that makes a visit, and the scrolls a party's arrival opens once it has
 * materialised, one after another: a new party's welcome (his scroll, then the cut to the
 * crystals while he gestures at them, the controls held meanwhile), the scroll for the level a
 * party comes back having beaten, and his word that his gateways have woken to a party owed it.
 * It borrows the world, the level's messages and his figure.
 */
class TowerWelcome {
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const TowerWelcome& value) {
        const auto& [cutCamera, cutTicks, due, next, intro] = value;
        visitor.values(cutCamera, cutTicks, due, next, intro);
    }

public:
    bool operator==(const TowerWelcome&) const = default;

    /** Where a new party's welcome has got to. */
    enum class Intro : u8 { None, Scroll, Crystal, Done };
    /** What the party's arrival opens, in this order: a new party's welcome, GarmMessage for a
     * party back from the Underworld's throne (do_players 0x80076B18-0x80076B94), then Sumner's
     * word that his gateways have woken. The original's arrival tries its modal scrolls in turn,
     * once for a tower loaded afresh (0x80344AF8, armed by LoadTowerAndSelect at 0x80053C44 and
     * spent at 0x80076B9C); it welcomes only a party's first coming to the tower (0x800A3A90),
     * which follows no level and no Garm beaten, so the welcome comes alone. */
    enum class Due : u8 { Nothing, Welcome, Garm, Gateways };

    static constexpr u32 kCrystalCamera = 198; ///< the trigger camera the welcome cuts to
    static constexpr s32 kCrystalTicks = 300;  ///< fifty frames of six ticks
    static constexpr s32 kSumnerSpot = 240;    ///< the id of the trigger before him
    /** The text id of Sumner's word that his gateways have woken, its one page. */
    static constexpr std::string_view kGatewayScroll = "gateway.announcement";

    /** Readies what the arrival opens, from the welcome asked for, the level the party has just
     * beaten and whether it is owed word of the gateways; a welcome due hides the crystals until
     * the cut reveals them. */
    void open(LevelWorld& world, bool welcome, std::optional<LevelBeaten> beaten = std::nullopt,
              bool gateways = false);
    void clear();

    /** A party is new to the tower while no class of any of its characters has experience. */
    static bool freshParty(std::span<const PartyMember> party);
    /** Whether `party` is owed word that the gateways have woken: one of it has beaten Garm and
     * not been told, and one of the worlds `gateways` offers is usable. Those are looked for only
     * then, and never without the link, as online. */
    static bool owesGateways(std::span<const PartyMember> party, TowerGateways* gateways);
    /** Every character of `players` still in the party that has beaten Garm has been told of the
     * gateways, in its record and in the one it came in with, which a level given up goes back
     * to; how many were told for the first time. */
    static usize tellGateways(std::span<PlayerRuntime> players);
    /** The standing player in the spot before Sumner, first of the party, if any: a body
     * waiting in the tower, unseen, visits nobody. */
    static std::optional<s32> visitorOf(const LevelTriggers& triggers,
                                        std::span<const PlayerRuntime> players);

    /** Once the party stands, the first scroll due opens (a welcome without its scroll goes on
     * to the cut); says which opened. */
    Due arrived(RenderDevice& device, LevelMessages& messages, const StringTable* strings,
                const WorldLayout& layout, SumnerFigure& sumner);
    /** The scroll has gone: after the welcome's, on to the crystals; after GarmMessage, on to
     * the gateways' when they are owed too. Says which scroll opened. */
    Due scrollClosed(RenderDevice& device, LevelMessages& messages, const StringTable* strings,
                     const WorldLayout& layout, SumnerFigure& sumner);
    /** Runs the cut down; true while it holds play. */
    bool hold(s32 ticks);

    Due due() const { return m_due; } ///< the scroll the arrival opens next
    Intro intro() const { return m_intro; }
    bool cutting() const { return m_intro == Intro::Crystal; }
    /** The cut's camera while it shows. */
    std::optional<WorldCamera> camera() const;

private:
    void startCrystalCut(const WorldLayout& layout, SumnerFigure& sumner);
    /** Opens the first scroll due that has something to show, passing over the rest. */
    Due openDue(RenderDevice& device, LevelMessages& messages, const StringTable* strings,
                const WorldLayout& layout, SumnerFigure& sumner);

    WorldCamera m_cutCamera;
    s32 m_cutTicks = 0;
    Due m_due = Due::Nothing;  ///< the scroll the arrival opens next
    Due m_next = Due::Nothing; ///< and the one after it
    Intro m_intro = Intro::None;
};

} // namespace gdl::game
