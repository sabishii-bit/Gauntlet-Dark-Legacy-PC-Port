#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCamera.h"

#include "game/players/Party.h"
#include "game/screens/LevelMessages.h"
#include "game/screens/PlayerRuntime.h"
#include "game/world/LevelWorld.h"
#include "game/world/SumnerFigure.h"

namespace gdl::game {

/**
 * The spot before Sumner that makes a visit, and the welcome a new party gets once it has
 * materialised (his scroll, then the cut to the crystals while he gestures at them, the
 * controls held meanwhile). It borrows the world, the level's messages and his figure.
 */
class TowerWelcome {
public:
    /** Where a new party's welcome has got to. */
    enum class Intro : u8 { None, Scroll, Crystal, Done };

    static constexpr u32 kCrystalCamera = 198; ///< the trigger camera the welcome cuts to
    static constexpr s32 kCrystalTicks = 300;  ///< fifty frames of six ticks
    static constexpr s32 kSumnerSpot = 240;    ///< the id of the trigger before him

    /** A welcome due hides the crystals until the cut reveals them. */
    void open(LevelWorld& world, bool welcome);
    void clear();

    /** A party is new to the tower while no class of any of its characters has experience. */
    static bool freshParty(std::span<const PartyMember> party);
    /** The player standing in the spot before Sumner, first of the party, if any. */
    static std::optional<s32> visitorOf(const LevelTriggers& triggers,
                                        std::span<const PlayerRuntime> players);

    /** Once the party stands, the welcome due starts: the scroll, or without it the cut. */
    void arrived(RenderDevice& device, LevelMessages& messages, const StringTable* strings,
                 const WorldLayout& layout, SumnerFigure& sumner);
    /** The scroll has gone: on to the crystals. */
    void scrollClosed(const WorldLayout& layout, SumnerFigure& sumner);
    /** Runs the cut down; true while it holds play. */
    bool hold(s32 ticks);

    Intro intro() const { return m_intro; }
    bool cutting() const { return m_intro == Intro::Crystal; }
    /** The cut's camera while it shows. */
    std::optional<WorldCamera> camera() const;

private:
    void startCrystalCut(const WorldLayout& layout, SumnerFigure& sumner);

    WorldCamera m_cutCamera;
    s32 m_cutTicks = 0;
    bool m_pending = false;
    Intro m_intro = Intro::None;
};

} // namespace gdl::game
