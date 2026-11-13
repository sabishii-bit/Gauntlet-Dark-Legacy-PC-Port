#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCamera.h"

#include "game/characters/Party.h"
#include "game/hud/LevelMessages.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayerRuntime.h"
#include "game/tower/LevelBeaten.h"
#include "game/tower/SumnerFigure.h"

namespace gdl::game {

/**
 * The spot before Sumner that makes a visit, and the scroll a party's arrival opens once it has
 * materialised: a new party's welcome (his scroll, then the cut to the crystals while he
 * gestures at them, the controls held meanwhile), or the scroll for the level a party comes
 * back having beaten. It borrows the world, the level's messages and his figure.
 */
class TowerWelcome {
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const TowerWelcome& value) {
        const auto& [cutCamera, cutTicks, due, intro] = value;
        visitor.values(cutCamera, cutTicks, due, intro);
    }

public:
    bool operator==(const TowerWelcome&) const = default;

    /** Where a new party's welcome has got to. */
    enum class Intro : u8 { None, Scroll, Crystal, Done };
    /** What the party's arrival opens: a new party's welcome, or GarmMessage for a party back
     * from the Underworld's throne (do_players 0x80076B18-0x80076B94). The original tries them
     * in that order but welcomes only a party's first coming to the tower (0x800A3A90), which
     * follows no level, so one at most is due. */
    enum class Due : u8 { Nothing, Welcome, Garm };

    static constexpr u32 kCrystalCamera = 198; ///< the trigger camera the welcome cuts to
    static constexpr s32 kCrystalTicks = 300;  ///< fifty frames of six ticks
    static constexpr s32 kSumnerSpot = 240;    ///< the id of the trigger before him

    /** Readies what the arrival opens, from the welcome asked for and the level the party has
     * just beaten; a welcome due hides the crystals until the cut reveals them. */
    void open(LevelWorld& world, bool welcome, std::optional<LevelBeaten> beaten = std::nullopt);
    void clear();

    /** A party is new to the tower while no class of any of its characters has experience. */
    static bool freshParty(std::span<const PartyMember> party);
    /** The standing player in the spot before Sumner, first of the party, if any: a body
     * waiting in the tower, unseen, visits nobody. */
    static std::optional<s32> visitorOf(const LevelTriggers& triggers,
                                        std::span<const PlayerRuntime> players);

    /** Once the party stands, what is due opens: the welcome's scroll (or without it the cut),
     * or the scroll for the level beaten. */
    void arrived(RenderDevice& device, LevelMessages& messages, const StringTable* strings,
                 const WorldLayout& layout, SumnerFigure& sumner);
    /** The scroll has gone: after the welcome's, on to the crystals. */
    void scrollClosed(const WorldLayout& layout, SumnerFigure& sumner);
    /** Runs the cut down; true while it holds play. */
    bool hold(s32 ticks);

    Due due() const { return m_due; }
    Intro intro() const { return m_intro; }
    bool cutting() const { return m_intro == Intro::Crystal; }
    /** The cut's camera while it shows. */
    std::optional<WorldCamera> camera() const;

private:
    void startCrystalCut(const WorldLayout& layout, SumnerFigure& sumner);

    WorldCamera m_cutCamera;
    s32 m_cutTicks = 0;
    Due m_due = Due::Nothing;
    Intro m_intro = Intro::None;
};

} // namespace gdl::game
