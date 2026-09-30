#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

#include "game/players/CharacterSave.h"
#include "game/players/Party.h"
#include "game/players/Progression.h"

namespace gdl::game {

/**
 * What the tower opens to a party, the way the original decides it for every joined
 * character together: which worlds are open (WorldOpen: the tower always, the temple, the
 * underworld and the battlefield by the bosses' shards and the runestones held, the rest by
 * the crystal gate their crystals open) and which portals may be passed (fn_8005B5B8: a
 * realm's first, then each by the level before it beaten; the temple's and underworld's by
 * their world; the battlefield's fourth by all thirteen runestones), which start marker a
 * party back from a realm may stand at, and whether the lifts down to the battlefield's
 * portals stand open (the battlefield's first level beaten). A shut portal wears the
 * EXIT_OFF figure and its glow (the tower's L1NSNC<letter><n>_ACTIVE object) is put out.
 */
class TowerAccess {
public:
    static constexpr s32 kWorldCount = 14;
    static constexpr s32 kTowerWorld = 13;
    static constexpr s32 kTempleWorld = 5;
    static constexpr s32 kUnderworldWorld = 6;
    static constexpr s32 kBattlefieldWorld = 8;
    static constexpr s32 kGarmGate = 3;              ///< the battlefield's fourth level
    static constexpr u16 kTempleShards = 0x1FE;      ///< the eight realms' bosses' shards
    static constexpr u16 kUnderworldShards = 0x3FE;  ///< and the temple's
    static constexpr u16 kUnderworldRunes = 0xFFF;   ///< the first twelve runestones
    static constexpr u16 kBattlefieldShards = 0x7FE; ///< and the underworld's
    static constexpr u16 kGarmRunes = 0x1FFF;        ///< all thirteen runestones
    /** The crystal gate each world's crystals open (lbl_80124D4C); nought is no gate. */
    static constexpr std::array<s32, kWorldCount> kWorldGates{0, 3, 2, 6, 5, 0, 0,
                                                              1, 0, 7, 8, 4, 0, 0};
    /** The world each of the tower's start markers stands among the portals of
     * (crystal_order). */
    static constexpr std::array<s32, kWorldCount> kMarkerWorlds{13, 7,  2, 1, 11, 4, 3,
                                                                9,  10, 5, 6, 8,  0, 0};
    /** The tower's lift triggers that stand open from the start once the battlefield's
     * first level is beaten (items.c 6956). */
    static constexpr std::array<s32, 2> kLiftTriggers{104, 199};
    static constexpr std::string_view kOffFigure = "EXIT_OFF";
    static constexpr char kWorldLetterBase = '@'; ///< a world's letter is this plus its id

    TowerAccess() = default;
    /** The answers for a party of saved characters (a Sumner among them passes every level
     * gate and crystal gate). */
    explicit TowerAccess(std::span<const CharacterSave> party);
    /** The same for a party in play, the fallen included (every joined player counts). */
    explicit TowerAccess(std::span<const PartyMember> party);
    /** The same for their progress alone. */
    explicit TowerAccess(std::span<const ClassProgress> party);

    bool worldOpen(s32 world) const;
    /** Whether the portal to level `gate` (from nought) of `world` may be passed. */
    bool portalOpen(s32 world, s32 gate) const;
    bool liftsOpen() const;
    /** Where a party bound for start marker `marker` stands: there, or at the entrance
     * (marker nought) when the world it stands among is shut (SetPlayerStartPos). */
    u32 startMarker(u32 marker) const;

    /** The tower's object that glows at a portal: L1NSNC<letter><n>_ACTIVE. */
    static std::string glowObjectName(s32 world, s32 gate);
    /** Whether a world object is one of those. */
    static bool isGlowObject(std::string_view name);
    /** The world an exit's letter names ('g' the town), or -1 for none. */
    static s32 worldOfLetter(char letter);

private:
    void add(const ClassProgress& progress, bool sumner);
    bool gateMet(s32 gate) const;

    std::array<s32, kRealmCount> m_crystals{}; ///< the most any member holds, by gate
    std::array<u8, kWorldCount> m_beaten{};
    std::array<bool, kRealmCount> m_gatesDone{}; ///< a member's count marked complete
    u16 m_shards = 0;
    u16 m_runes = 0;
    bool m_anyone = false;
    bool m_sumner = false;
};

} // namespace gdl::game
