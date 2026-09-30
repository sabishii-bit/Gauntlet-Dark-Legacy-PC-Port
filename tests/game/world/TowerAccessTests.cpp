#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"
#include "game/players/Party.h"
#include "game/players/Progression.h"
#include "game/world/TowerAccess.h"

namespace {

using namespace gdl;
using namespace gdl::game;

constexpr s32 kTown = 7;
constexpr s32 kMountain = 2;
constexpr s32 kTownGate = 1; ///< the crystal gate the town's crystals open, wanting 15

TEST_CASE("the worlds open by their crystal gate, the tower always",
          "[game][world][tower-access]") {
    const TowerAccess nobody{std::span<const ClassProgress>{}};
    REQUIRE(nobody.worldOpen(TowerAccess::kTowerWorld));
    REQUIRE_FALSE(nobody.worldOpen(kTown));
    REQUIRE_FALSE(nobody.worldOpen(kMountain));
    REQUIRE_FALSE(nobody.worldOpen(-1));
    REQUIRE_FALSE(nobody.worldOpen(14));
    std::vector<ClassProgress> party(2);
    party[1].crystals[kTownGate] = 14;
    REQUIRE_FALSE(TowerAccess{party}.worldOpen(kTown));
    // The most anyone holds counts, not what all hold.
    party[1].crystals[kTownGate] = 15;
    REQUIRE(TowerAccess{party}.worldOpen(kTown));
    REQUIRE_FALSE(TowerAccess{party}.worldOpen(kMountain));
    // A count marked complete (negative) meets the gate whatever the best.
    party[1].crystals[kTownGate] = 0;
    party[0].crystals[kTownGate] = -1;
    REQUIRE(TowerAccess{party}.worldOpen(kTown));
}

TEST_CASE("the temple, the underworld and the battlefield open by shards and runestones",
          "[game][world][tower-access]") {
    std::vector<ClassProgress> party(1);
    Relics& relics = party[0].relics;
    relics.shards = TowerAccess::kTempleShards & ~static_cast<u16>(1U << 8U);
    REQUIRE_FALSE(TowerAccess{party}.worldOpen(TowerAccess::kTempleWorld));
    REQUIRE_FALSE(TowerAccess{party}.portalOpen(TowerAccess::kTempleWorld, 0));
    relics.shards = TowerAccess::kTempleShards;
    REQUIRE(TowerAccess{party}.worldOpen(TowerAccess::kTempleWorld));
    REQUIRE(TowerAccess{party}.portalOpen(TowerAccess::kTempleWorld, 0));
    REQUIRE_FALSE(TowerAccess{party}.worldOpen(TowerAccess::kUnderworldWorld));
    relics.shards = TowerAccess::kUnderworldShards;
    REQUIRE_FALSE(TowerAccess{party}.worldOpen(TowerAccess::kUnderworldWorld)); // no runestones
    relics.runes = TowerAccess::kUnderworldRunes;
    REQUIRE(TowerAccess{party}.worldOpen(TowerAccess::kUnderworldWorld));
    REQUIRE(TowerAccess{party}.portalOpen(TowerAccess::kUnderworldWorld, 0));
    REQUIRE_FALSE(TowerAccess{party}.worldOpen(TowerAccess::kBattlefieldWorld));
    relics.shards = TowerAccess::kBattlefieldShards;
    REQUIRE(TowerAccess{party}.worldOpen(TowerAccess::kBattlefieldWorld));
    // The shards may be spread over the party.
    std::vector<ClassProgress> pair(2);
    pair[0].relics.shards = 0x0FE;
    pair[1].relics.shards = 0x300;
    REQUIRE(TowerAccess{pair}.worldOpen(TowerAccess::kUnderworldWorld) == false);
    pair[1].relics.runes = TowerAccess::kUnderworldRunes;
    REQUIRE(TowerAccess{pair}.worldOpen(TowerAccess::kUnderworldWorld));
}

TEST_CASE("a realm's portals open one by one as the level before is beaten",
          "[game][world][tower-access]") {
    std::vector<ClassProgress> party(2);
    const auto access = [&] { return TowerAccess{party}; };
    // The first portal stands whatever the crystals say: the doors gate the realm.
    REQUIRE(access().portalOpen(kTown, 0));
    REQUIRE_FALSE(access().portalOpen(kTown, 1));
    REQUIRE_FALSE(access().portalOpen(kTown, 4));
    party[1].levels.recordBeaten(kTown, 0);
    REQUIRE(access().portalOpen(kTown, 1));
    REQUIRE_FALSE(access().portalOpen(kTown, 2));
    // Beaten by anyone in the party.
    party[0].levels.recordBeaten(kTown, 1);
    REQUIRE(access().portalOpen(kTown, 2));
    REQUIRE_FALSE(access().portalOpen(kMountain, 1));
    REQUIRE_FALSE(access().portalOpen(-1, 0));
    REQUIRE_FALSE(access().portalOpen(kTown, -1));
}

TEST_CASE("the battlefield's portals want its world, then Garm's all thirteen runestones",
          "[game][world][tower-access]") {
    std::vector<ClassProgress> party(1);
    ClassProgress& progress = party[0];
    progress.levels.recordBeaten(TowerAccess::kBattlefieldWorld, 0);
    progress.levels.recordBeaten(TowerAccess::kBattlefieldWorld, 1);
    progress.levels.recordBeaten(TowerAccess::kBattlefieldWorld, 2);
    REQUIRE_FALSE(TowerAccess{party}.portalOpen(TowerAccess::kBattlefieldWorld, 0));
    progress.relics.shards = TowerAccess::kBattlefieldShards;
    REQUIRE(TowerAccess{party}.portalOpen(TowerAccess::kBattlefieldWorld, 0));
    REQUIRE(TowerAccess{party}.portalOpen(TowerAccess::kBattlefieldWorld, 1));
    REQUIRE(TowerAccess{party}.portalOpen(TowerAccess::kBattlefieldWorld, 2));
    REQUIRE_FALSE(
        TowerAccess{party}.portalOpen(TowerAccess::kBattlefieldWorld, TowerAccess::kGarmGate));
    progress.relics.runes = TowerAccess::kUnderworldRunes;
    REQUIRE_FALSE(
        TowerAccess{party}.portalOpen(TowerAccess::kBattlefieldWorld, TowerAccess::kGarmGate));
    progress.relics.runes = TowerAccess::kGarmRunes;
    REQUIRE(TowerAccess{party}.portalOpen(TowerAccess::kBattlefieldWorld, TowerAccess::kGarmGate));
    // Garm's portal goes by the runestones alone, not by the third level beaten.
    ClassProgress fresh;
    fresh.relics.shards = TowerAccess::kBattlefieldShards;
    fresh.relics.runes = TowerAccess::kGarmRunes;
    const std::array<ClassProgress, 1> only{fresh};
    REQUIRE(TowerAccess{only}.portalOpen(TowerAccess::kBattlefieldWorld, TowerAccess::kGarmGate));
    REQUIRE_FALSE(TowerAccess{only}.portalOpen(TowerAccess::kBattlefieldWorld, 1));
}

TEST_CASE("the lifts stand open once the battlefield's first level is beaten",
          "[game][world][tower-access]") {
    std::vector<ClassProgress> party(1);
    REQUIRE_FALSE(TowerAccess{party}.liftsOpen());
    party[0].levels.recordBeaten(TowerAccess::kBattlefieldWorld, 1);
    REQUIRE_FALSE(TowerAccess{party}.liftsOpen());
    party[0].levels.recordBeaten(TowerAccess::kBattlefieldWorld, 0);
    REQUIRE(TowerAccess{party}.liftsOpen());
    REQUIRE(TowerAccess::kLiftTriggers == std::array<s32, 2>{104, 199});
}

TEST_CASE("a Sumner in the party passes every level and crystal gate",
          "[game][world][tower-access]") {
    std::vector<CharacterSave> party(2);
    party[1].character = kSumnerClass;
    const TowerAccess access{party};
    REQUIRE(access.worldOpen(kTown));
    REQUIRE(access.worldOpen(kMountain));
    REQUIRE(access.portalOpen(kTown, 4));
    REQUIRE(access.liftsOpen());
    // The worlds that want shards and runestones still want them.
    REQUIRE_FALSE(access.worldOpen(TowerAccess::kTempleWorld));
    REQUIRE_FALSE(access.portalOpen(TowerAccess::kBattlefieldWorld, 0));
    party[1].character = 0;
    REQUIRE_FALSE(TowerAccess{party}.worldOpen(kTown));
    // A party in play counts every member, the fallen too.
    std::vector<PartyMember> members(1);
    members[0].fallen = true;
    members[0].save.progress().levels.recordBeaten(kTown, 0);
    REQUIRE(TowerAccess{members}.portalOpen(kTown, 1));
}

TEST_CASE("a party back from a shut world stands at the entrance", "[game][world][tower-access]") {
    std::vector<ClassProgress> party(1);
    REQUIRE(TowerAccess{party}.startMarker(0) == 0);
    REQUIRE(TowerAccess{party}.startMarker(1) == 0); // the town's marker, its world shut
    party[0].crystals[kTownGate] = 15;
    REQUIRE(TowerAccess{party}.startMarker(1) == 1);
    REQUIRE(TowerAccess{party}.startMarker(2) == 0); // the mountain's
    REQUIRE(TowerAccess{party}.startMarker(14) == 0);
}

TEST_CASE("the tower's portal glows are named by world letter and level",
          "[game][world][tower-access]") {
    REQUIRE(TowerAccess::glowObjectName(kMountain, 1) == "L1NSNCB2_ACTIVE");
    REQUIRE(TowerAccess::glowObjectName(kTown, 4) == "L1NSNCG5_ACTIVE");
    REQUIRE(TowerAccess::isGlowObject("L1NSNCB2_ACTIVE"));
    REQUIRE(TowerAccess::isGlowObject("L1NSNCK5_ACTIVE"));
    REQUIRE_FALSE(TowerAccess::isGlowObject("L1NSNC_LINE14"));
    REQUIRE_FALSE(TowerAccess::isGlowObject("L1NSNCCENTERPIE"));
    REQUIRE_FALSE(TowerAccess::isGlowObject("L1NSNCGROUP2#0"));
    REQUIRE_FALSE(TowerAccess::isGlowObject("L1NSNCZ1_ACTIVE"));
    REQUIRE(TowerAccess::worldOfLetter('g') == kTown);
    REQUIRE(TowerAccess::worldOfLetter('B') == kMountain);
    REQUIRE(TowerAccess::worldOfLetter('k') == 11);
    REQUIRE(TowerAccess::worldOfLetter('L') == -1);
    REQUIRE(TowerAccess::worldOfLetter('@') == -1);
}

} // namespace
