#include <array>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "TestSupport.h"
#include "expansions/n64/hud/LegendsN64Plaque.h"
#include "game/hud/HudView.h"
#include "game/level/LevelCatalog.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** The exits of the town's first level and the mountain's, as the N64 tower's pads name them. */
constexpr u32 kTownFirst = 0x0701;
constexpr u32 kMountainFirst = 0x0201;
/** A secret level's return, a code no picture names. */
constexpr u32 kSecretReturn = 0x1302;
/** What fades() records for a step with no plaque. */
constexpr s32 kNoPlaque = -1;

/** The plaque's transparency at each of `steps` steps of a seat on the exit `wanted` names (an
 * N64 exit with its picture), kNoPlaque for a step it shows none. */
std::vector<s32> fades(LegendsN64Plaque& plaque, std::optional<u32> wanted, s32 steps) {
    std::vector<s32> shown;
    for (s32 step = 0; step < steps; ++step) {
        plaque.step(wanted, nullptr, true);
        shown.push_back(plaque.shown() ? s32{plaque.shown()->transparency} : kNoPlaque);
    }
    return shown;
}

/** A plaque at rest over a seat standing on `code`'s exit. */
LegendsN64Plaque restingOn(u32 code) {
    LegendsN64Plaque plaque;
    plaque.snap(code, nullptr, true);
    REQUIRE(plaque.shown());
    REQUIRE(plaque.shown()->transparency == 0x1f);
    return plaque;
}

/** The code `plaque` was made for. */
u32 codeOf(const LegendsN64Plaque& plaque) {
    REQUIRE(plaque.shown());
    return plaque.shown()->code;
}

} // namespace

TEST_CASE("a plaque fades in over six steps and rests", "[legends][n64-hud][n64-plaque]") {
    LegendsN64Plaque plaque;
    CHECK_FALSE(plaque.shown());
    CHECK_FALSE(plaque.answering());
    // Made at 0xdf as the seat steps on, then 0x20 clearer a step until it rests at 0x1f.
    CHECK(fades(plaque, kTownFirst, 8) ==
          std::vector<s32>{0xdf, 0xbf, 0x9f, 0x7f, 0x5f, 0x3f, 0x1f, 0x1f});
    CHECK(codeOf(plaque) == kTownFirst);
    CHECK_FALSE(plaque.shown()->darkLegacy);
    CHECK(plaque.answering() == kTownFirst);
}

TEST_CASE("a plaque cleared from rest fades out, then the shown code goes, then the plaque",
          "[legends][n64-hud][n64-plaque]") {
    LegendsN64Plaque plaque = restingOn(kTownFirst);
    // Off the exit it clouds over by 0x40 a step, still answering to it...
    CHECK(fades(plaque, std::nullopt, 3) == std::vector<s32>{0x5f, 0x9f, 0xdf});
    CHECK(plaque.answering() == kTownFirst);
    // ...lets it go once past 0xbe, still showing...
    CHECK(fades(plaque, std::nullopt, 1) == std::vector<s32>{0xdf});
    CHECK_FALSE(plaque.answering());
    CHECK(codeOf(plaque) == kTownFirst);
    // ...and goes once past 0xde; off every exit nothing more happens.
    CHECK(fades(plaque, std::nullopt, 3) == std::vector<s32>{kNoPlaque, kNoPlaque, kNoPlaque});
    CHECK_FALSE(plaque.answering());
}

TEST_CASE("a plaque cleared early is deleted one step after it passes the out mark",
          "[legends][n64-hud][n64-plaque]") {
    LegendsN64Plaque plaque;
    REQUIRE(fades(plaque, kTownFirst, 6).back() == 0x3f);
    // From 0x3f it clouds to 0x7f and 0xbf, lets the exit go past 0xbe, then clouds on by 0x20
    // and goes past 0xde.
    CHECK(fades(plaque, std::nullopt, 2) == std::vector<s32>{0x7f, 0xbf});
    CHECK(plaque.answering() == kTownFirst);
    CHECK(fades(plaque, std::nullopt, 1) == std::vector<s32>{0xbf});
    CHECK_FALSE(plaque.answering());
    CHECK(fades(plaque, std::nullopt, 2) == std::vector<s32>{0xdf, kNoPlaque});
}

TEST_CASE("a new exit replaces the old plaque only once it has faded",
          "[legends][n64-hud][n64-plaque]") {
    LegendsN64Plaque plaque = restingOn(kTownFirst);
    // Onto another exit, the old plaque clouds over first, still its own...
    CHECK(fades(plaque, kMountainFirst, 3) == std::vector<s32>{0x5f, 0x9f, 0xdf});
    CHECK(codeOf(plaque) == kTownFirst);
    // ...then lets its exit go...
    CHECK(fades(plaque, kMountainFirst, 1) == std::vector<s32>{0xdf});
    CHECK(codeOf(plaque) == kTownFirst);
    CHECK_FALSE(plaque.answering());
    // ...and the new exit's plaque takes its place, made at 0xdf, and clears to rest.
    CHECK(fades(plaque, kMountainFirst, 1) == std::vector<s32>{0xdf});
    CHECK(codeOf(plaque) == kMountainFirst);
    CHECK(plaque.answering() == kMountainFirst);
    CHECK(fades(plaque, kMountainFirst, 7) ==
          std::vector<s32>{0xbf, 0x9f, 0x7f, 0x5f, 0x3f, 0x1f, 0x1f});
}

TEST_CASE("an exit returned to before its plaque has gone fades back in from where it is",
          "[legends][n64-hud][n64-plaque]") {
    LegendsN64Plaque plaque = restingOn(kTownFirst);
    SECTION("while it clouds over") {
        REQUIRE(fades(plaque, std::nullopt, 2) == std::vector<s32>{0x5f, 0x9f});
        // Still answering to the exit, it clears from 0x9f.
        CHECK(fades(plaque, kTownFirst, 5) == std::vector<s32>{0x7f, 0x5f, 0x3f, 0x1f, 0x1f});
        CHECK(codeOf(plaque) == kTownFirst);
    }
    SECTION("once it has let the exit go") {
        REQUIRE(fades(plaque, std::nullopt, 4) == std::vector<s32>{0x5f, 0x9f, 0xdf, 0xdf});
        REQUIRE_FALSE(plaque.answering());
        // The exit is answered afresh: a plaque made where the old one stands, at 0xdf.
        CHECK(fades(plaque, kTownFirst, 3) == std::vector<s32>{0xdf, 0xbf, 0x9f});
        CHECK(codeOf(plaque) == kTownFirst);
        CHECK(plaque.answering() == kTownFirst);
    }
}

TEST_CASE("a rewind snaps the plaque to what the seat stands on",
          "[legends][n64-hud][n64-plaque]") {
    LegendsN64Plaque plaque;
    // Half faded in, a rewind finds the seat on the exit: at rest.
    REQUIRE(fades(plaque, kTownFirst, 2) == std::vector<s32>{0xdf, 0xbf});
    plaque.snap(kTownFirst, nullptr, true);
    REQUIRE(plaque.shown());
    CHECK(plaque.shown()->transparency == 0x1f);
    CHECK(plaque.answering() == kTownFirst);
    // Clouding over, a rewind finds it back on the exit: at rest again.
    REQUIRE(fades(plaque, std::nullopt, 1) == std::vector<s32>{0x5f});
    plaque.snap(kTownFirst, nullptr, true);
    CHECK(plaque.shown()->transparency == 0x1f);
    // On another exit at once, at rest on it, the old one gone.
    plaque.snap(kMountainFirst, nullptr, true);
    CHECK(codeOf(plaque) == kMountainFirst);
    CHECK(plaque.shown()->transparency == 0x1f);
    CHECK(plaque.answering() == kMountainFirst);
    // The steps after go on from there.
    CHECK(fades(plaque, std::nullopt, 1) == std::vector<s32>{0x5f});
    // Off every exit, nothing.
    plaque.snap(std::nullopt, nullptr, true);
    CHECK_FALSE(plaque.shown());
    CHECK_FALSE(plaque.answering());
    CHECK(fades(plaque, std::nullopt, 2) == std::vector<s32>{kNoPlaque, kNoPlaque});
}

TEST_CASE("an exit without a picture shows no plaque and gives way to the next at once",
          "[legends][n64-hud][n64-plaque]") {
    LegendsN64Plaque plaque;
    // FUN_e0058560 makes nothing for a code it has no picture of, but FUN_e0058620 answers the
    // code all the same, its byte at 0xdf, and has nothing to fade.
    for (s32 step = 0; step < 8; ++step) {
        CAPTURE(step);
        plaque.step(kSecretReturn, nullptr, false);
        CHECK_FALSE(plaque.shown());
        CHECK(plaque.answering() == kSecretReturn);
    }
    // On a pictured exit, the byte already past 0xbe lets the code go at once, and the next
    // step makes the new plaque.
    plaque.step(kTownFirst, nullptr, true);
    CHECK_FALSE(plaque.shown());
    CHECK_FALSE(plaque.answering());
    CHECK(fades(plaque, kTownFirst, 2) == std::vector<s32>{0xdf, 0xbf});
    // A plaque still clouding over goes as soon as an exit without a picture is answered.
    plaque = restingOn(kTownFirst);
    std::vector<s32> shown;
    for (s32 step = 0; step < 5; ++step) {
        plaque.step(kSecretReturn, nullptr, false);
        shown.push_back(plaque.shown() ? s32{plaque.shown()->transparency} : kNoPlaque);
    }
    CHECK(shown == std::vector<s32>{0x5f, 0x9f, 0xdf, 0xdf, kNoPlaque});
    CHECK(plaque.answering() == kSecretReturn);
    // Snapped onto one, it shows nothing, and the next exit takes over as quickly.
    plaque.snap(kSecretReturn, nullptr, false);
    CHECK_FALSE(plaque.shown());
    CHECK(plaque.answering() == kSecretReturn);
    CHECK(fades(plaque, kMountainFirst, 2) == std::vector<s32>{kNoPlaque, 0xdf});
}

TEST_CASE("an N64 exit's plaque is the picture its code names", "[legends][n64-hud][n64-plaque]") {
    // e00aae08's "LNAME_%02.2x%02.2x" of the code's realm byte and its level byte.
    CHECK(legendsN64PlaqueArt(kTownFirst) == "LNAME_0701");
    CHECK(legendsN64PlaqueArt(0x1103) == "LNAME_1103");
    CHECK(legendsN64PlaqueArt(0x0f02) == "LNAME_0f02");
    CHECK(legendsN64PlaqueArt(0xffffffd0) == "LNAME_ffd0");
    CHECK(legendsN64PlaqueArt(0) == "LNAME_0000");
    CHECK(legendsN64PlaqueArt(kMountainFirst).starts_with(kLegendsN64PlaquePrefix));
}

TEST_CASE("the port's plaques name each realm as Dark Legacy's scrolls do",
          "[legends][n64-hud][n64-plaque]") {
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    // Each realm's name is the phrase a retail scroll page names it by.
    struct Source {
        s32 realm = 0;
        std::string_view page;
    };
    constexpr std::array<Source, 11> kSources{{{.realm = 1, .page = "scroll.unlocklevel.4"},
                                               {.realm = 2, .page = "scroll.unlocklevel.3"},
                                               {.realm = 3, .page = "scroll.unlocklevel.7"},
                                               {.realm = 4, .page = "scroll.unlocklevel.6"},
                                               {.realm = 5, .page = "scroll.allshards.2"},
                                               {.realm = 6, .page = "scroll.all12runesno.2"},
                                               {.realm = 7, .page = "scroll.unlocklevel.2"},
                                               {.realm = 8, .page = "scroll.needgargitems.3"},
                                               {.realm = 9, .page = "scroll.unlocklevel.8"},
                                               {.realm = 10, .page = "scroll.unlocklevel.9"},
                                               {.realm = 11, .page = "scroll.unlocklevel.5"}}};
    std::set<std::string> names;
    for (const Source& source : kSources) {
        CAPTURE(source.realm, source.page);
        const std::optional<std::string> key = darkLegacyRealmNameText(source.realm);
        REQUIRE(key);
        CHECK(*key == std::format("hud.realm.{}", source.realm));
        REQUIRE(strings.has(*key));
        REQUIRE(strings.has(source.page));
        const std::string_view name = strings.get(*key);
        REQUIRE_FALSE(name.empty());
        CHECK(strings.get(source.page).find(name) != std::string_view::npos);
        names.emplace(name);
    }
    // Every realm its own.
    CHECK(names.size() == kSources.size());
    // The tower, the secret realm and anything else have none.
    for (const s32 realm : {-1, 0, LevelRef::kSecretRealm, LevelRef::kTowerRealm, 14}) {
        CAPTURE(realm);
        CHECK_FALSE(darkLegacyRealmNameText(realm));
    }
}

TEST_CASE("a destination without a realm name shows its title alone, and the tower none",
          "[legends][n64-hud][n64-plaque]") {
    // A Dark Legacy exit's plaque is the port's, naming its level, in capitals.
    LevelRef fields;
    fields.realmId = 7;
    fields.title = "Poisoned Fields";
    LegendsN64Plaque plaque;
    plaque.step(kDarkLegacyPadIdentityBase + 1, &fields, true);
    REQUIRE(plaque.shown());
    CHECK(*plaque.shown() == LegendsN64Plaque::Shown{.code = kDarkLegacyPadIdentityBase + 1,
                                                     .transparency = 0xdf,
                                                     .darkLegacy = true,
                                                     .realmId = 7,
                                                     .title = "POISONED FIELDS"});
    CHECK(darkLegacyRealmNameText(plaque.shown()->realmId) == "hud.realm.7");

    // A secret level's realm has no name: its plaque has its title alone.
    LevelRef hidden;
    hidden.realmId = LevelRef::kSecretRealm;
    hidden.title = "Hidden Valley";
    LegendsN64Plaque secret;
    secret.step(kDarkLegacyPadIdentityBase + 2, &hidden, true);
    REQUIRE(secret.shown());
    CHECK(secret.shown()->darkLegacy);
    CHECK(secret.shown()->title == "HIDDEN VALLEY");
    CHECK_FALSE(darkLegacyRealmNameText(secret.shown()->realmId));

    // An exit back to the tower makes none, however long the seat stands on it, nor snapped.
    const LevelRef tower = LevelRef::tower();
    LegendsN64Plaque towerward;
    for (s32 step = 0; step < 8; ++step) {
        CAPTURE(step);
        towerward.step(kDarkLegacyPadIdentityBase, &tower, true);
        CHECK_FALSE(towerward.shown());
        CHECK_FALSE(towerward.answering());
    }
    towerward.snap(kDarkLegacyPadIdentityBase, &tower, true);
    CHECK_FALSE(towerward.shown());
    CHECK_FALSE(towerward.answering());
    // Stepping from another exit onto one back to the tower is leaving every exit.
    plaque.snap(kDarkLegacyPadIdentityBase + 1, &fields, true);
    std::vector<s32> shown;
    for (s32 step = 0; step < 5; ++step) {
        plaque.step(kDarkLegacyPadIdentityBase, &tower, true);
        shown.push_back(plaque.shown() ? s32{plaque.shown()->transparency} : kNoPlaque);
    }
    CHECK(shown == std::vector<s32>{0x5f, 0x9f, 0xdf, 0xdf, kNoPlaque});
}
