#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "LogCapture.h"
#include "TestSupport.h"
#include "expansions/shared/app/LegendsHudStyles.h"
#include "expansions/shared/characters/LegendsProgress.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "game/config/GameConfig.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** What the N64 look cannot be drawn without, under n64/extracted. */
constexpr std::array<std::string_view, 4> kN64Files{"hud/objects.wad", "Font/4x6font.fnt",
                                                    "Font/glfm3.fnt", "Font/numfont.FNT"};
/** What the Dreamcast look cannot be drawn without, under dreamcast/extracted/native. */
constexpr std::array<std::string_view, 5> kDreamcastFiles{
    "static/objects.rom", "static/textures.rom", "fonts/8hi_fonts5.fnt", "fonts/score.fnt",
    "fonts/font32.fnt"};

/** A small native file at `path`, or an empty one, its folders made. */
void fixture(const std::filesystem::path& path, std::string_view bytes = "native fixture") {
    std::filesystem::create_directories(path.parent_path());
    writeTextFile(path, bytes);
}

/** An N64 extraction at `extracted`: a map, as legendsMaps counts one, and every file the N64
 * look needs. */
void n64At(const std::filesystem::path& extracted) {
    fixture(extracted / "levels/tower2/worlds.wad");
    fixture(extracted / "levels/tower2/objects.wad");
    for (const std::string_view name : kN64Files) {
        fixture(extracted / name);
    }
}

/** A Dreamcast extraction at `extracted`, its native files under native: a map and every file
 * the Dreamcast look needs. */
void dreamcastAt(const std::filesystem::path& extracted) {
    const auto native = extracted / "native";
    fixture(native / "levels/levelt1/worlds.rom");
    fixture(native / "levels/levelt1/objects.rom");
    for (const std::string_view name : kDreamcastFiles) {
        fixture(native / name);
    }
}

/** Where a game installed in `install` looks, its data in `install/Gauntlet`. */
LegendsHudRoots rootsOf(const std::filesystem::path& install) {
    return {.assetRoot = install / "Gauntlet",
            .executableDirectory = install / "bin",
            .workingDirectory = install / "working"};
}

/** Whether `found` is the folder `expected`. */
bool sameFolder(const std::optional<std::filesystem::path>& found,
                const std::filesystem::path& expected) {
    std::error_code error;
    return found.has_value() && std::filesystem::equivalent(*found, expected, error);
}

/** What opening look `id` under `roots` gave, and the warnings it wrote. */
struct Opening {
    std::unique_ptr<HudPainter> painter;
    std::vector<std::string> warnings;
};

/** Opens look `id` for a party of one, borrowing from `darkLegacy`, which outlives the opening. */
Opening openingOf(std::string_view id, const LegendsHudRoots& roots, test::FakeRenderDevice& device,
                  DarkLegacyHudPainter& darkLegacy) {
    const std::array party{HudMember{.seat = 1, .classIndex = 0, .color = 2}};
    Opening opening;
    opening.warnings = test::warningsWhile(
        [&] { opening.painter = openLegendsHud(id, device, darkLegacy, party, roots); });
    return opening;
}

} // namespace

TEST_CASE("a GameCube-only installation can draw no Legends HUD look", "[legends][hud-style]") {
    const auto install = test::scratchDirectory("legends-hud-none");
    std::filesystem::create_directories(install / "Gauntlet");
    const LegendsHudRoots roots = rootsOf(install);
    CHECK_FALSE(legendsHudRoot(LegendsPlatform::N64, roots));
    CHECK_FALSE(legendsHudRoot(LegendsPlatform::Dreamcast, roots));
    CHECK_FALSE(legendsHudFilesFound(kLegendsN64HudStyle, roots));
    CHECK_FALSE(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));
    // Whichever looks this build has painters for are offered, and none of them can be drawn.
    CHECK(std::ranges::none_of(legendsHudStyles(roots), &HudStyleOffer::usable));

    // Asked of roots that are not there at all, it still answers.
    const LegendsHudRoots nowhere{.assetRoot = install / "missing",
                                  .executableDirectory = {},
                                  .workingDirectory = install / "missing too"};
    CHECK_FALSE(legendsHudRoot(LegendsPlatform::N64, nowhere));
    CHECK_FALSE(legendsHudFilesFound(kLegendsDreamcastHudStyle, nowhere));
    CHECK(std::ranges::none_of(legendsHudStyles(nowhere), &HudStyleOffer::usable));

    // The looks' files alone, without their campaigns' maps, are no installation of either.
    for (const std::string_view name : kN64Files) {
        fixture(install / "n64/extracted" / name);
    }
    for (const std::string_view name : kDreamcastFiles) {
        fixture(install / "dreamcast/extracted/native" / name);
    }
    CHECK_FALSE(legendsHudRoot(LegendsPlatform::N64, roots));
    CHECK_FALSE(legendsHudFilesFound(kLegendsN64HudStyle, roots));
    CHECK_FALSE(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));
}

TEST_CASE("each Legends HUD look needs its own files, and one look's fault hides no other",
          "[legends][hud-style]") {
    // Each look names its own files; Dark Legacy's look and an id no look has name none.
    CHECK(std::ranges::equal(legendsHudRequiredFiles(kLegendsN64HudStyle), kN64Files));
    CHECK(std::ranges::equal(legendsHudRequiredFiles(kLegendsDreamcastHudStyle), kDreamcastFiles));
    CHECK(legendsHudRequiredFiles(kDarkLegacyHudStyle).empty());
    CHECK(legendsHudRequiredFiles("future").empty());

    const auto install = test::scratchDirectory("legends-hud-files");
    n64At(install / "n64/extracted");
    dreamcastAt(install / "dreamcast/extracted");
    const LegendsHudRoots roots = rootsOf(install);
    REQUIRE(legendsHudFilesFound(kLegendsN64HudStyle, roots));
    REQUIRE(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));
    CHECK_FALSE(legendsHudFilesFound(kDarkLegacyHudStyle, roots));
    CHECK_FALSE(legendsHudFilesFound("future", roots));

    const auto* removed =
        GENERATE("n64/extracted/hud/objects.wad", "n64/extracted/Font/4x6font.fnt",
                 "n64/extracted/Font/glfm3.fnt", "n64/extracted/Font/numfont.FNT",
                 "dreamcast/extracted/native/static/objects.rom",
                 "dreamcast/extracted/native/static/textures.rom",
                 "dreamcast/extracted/native/fonts/8hi_fonts5.fnt",
                 "dreamcast/extracted/native/fonts/score.fnt",
                 "dreamcast/extracted/native/fonts/font32.fnt");
    CAPTURE(removed);
    REQUIRE(std::filesystem::remove(install / removed));
    const bool n64 = std::string_view{removed}.starts_with("n64/");
    CHECK(legendsHudFilesFound(kLegendsN64HudStyle, roots) == !n64);
    CHECK(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots) == n64);
    // The campaign itself is still found: its maps are whole.
    CHECK(legendsHudRoot(n64 ? LegendsPlatform::N64 : LegendsPlatform::Dreamcast, roots));
}

TEST_CASE("an empty file counts as missing", "[legends][hud-style]") {
    const auto install = test::scratchDirectory("legends-hud-empty");
    n64At(install / "n64/extracted");
    dreamcastAt(install / "dreamcast/extracted");
    const LegendsHudRoots roots = rootsOf(install);
    // An extraction cut short leaves the font there, holding nothing.
    fixture(install / "n64/extracted/Font/numfont.FNT", "");
    CHECK_FALSE(legendsHudFilesFound(kLegendsN64HudStyle, roots));
    CHECK(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));
    fixture(install / "dreamcast/extracted/native/static/textures.rom", "");
    CHECK_FALSE(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));
}

TEST_CASE("a switched-off extraction is not found", "[legends][hud-style]") {
    const auto install = test::scratchDirectory("legends-hud-off");
    // The launcher switches an expansion off by renaming its extraction.
    n64At(install / "n64/extracted.off");
    dreamcastAt(install / "dreamcast/extracted");
    const LegendsHudRoots roots = rootsOf(install);
    CHECK_FALSE(legendsHudRoot(LegendsPlatform::N64, roots));
    CHECK_FALSE(legendsHudFilesFound(kLegendsN64HudStyle, roots));
    CHECK(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));

    std::filesystem::rename(install / "dreamcast/extracted", install / "dreamcast/extracted.off");
    CHECK_FALSE(legendsHudRoot(LegendsPlatform::Dreamcast, roots));
    CHECK_FALSE(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));
}

TEST_CASE("the HUD art is found beside the maps", "[legends][hud-style]") {
    const auto scratch = test::scratchDirectory("legends-hud-roots");
    const auto install = scratch / "install";
    LegendsHudRoots roots;
    std::filesystem::path n64;
    std::filesystem::path dreamcast;
    SECTION("under a development checkout's assets") {
        n64 = install / "assets/n64/extracted";
        dreamcast = install / "assets/dreamcast/extracted";
        roots = LegendsHudRoots{.assetRoot = install / "assets/GUNE5D/Gauntlet",
                                .executableDirectory = install / "build/bin",
                                .workingDirectory = install / "working"};
    }
    SECTION("beside the executable, where the launcher installs them") {
        n64 = install / "n64/extracted";
        dreamcast = install / "dreamcast/extracted";
        roots = LegendsHudRoots{.assetRoot = scratch / "elsewhere/Gauntlet",
                                .executableDirectory = install,
                                .workingDirectory = scratch / "elsewhere/working"};
    }
    SECTION("both, the configured assets' before the executable's") {
        n64 = install / "assets/n64/extracted";
        dreamcast = install / "assets/dreamcast/extracted";
        n64At(scratch / "bin/n64/extracted");
        dreamcastAt(scratch / "bin/dreamcast/extracted");
        roots = LegendsHudRoots{.assetRoot = install / "assets/GUNE5D/Gauntlet",
                                .executableDirectory = scratch / "bin",
                                .workingDirectory = install / "working"};
    }
    n64At(n64);
    dreamcastAt(dreamcast);
    CHECK(sameFolder(legendsHudRoot(LegendsPlatform::N64, roots), n64));
    CHECK(sameFolder(legendsHudRoot(LegendsPlatform::Dreamcast, roots), dreamcast / "native"));
    CHECK(legendsHudFilesFound(kLegendsN64HudStyle, roots));
    CHECK(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));
}

TEST_CASE("a look is offered only once its painter is built", "[legends][hud-style]") {
    const auto install = test::scratchDirectory("legends-hud-unbuilt");
    n64At(install / "n64/extracted");
    dreamcastAt(install / "dreamcast/extracted");
    const LegendsHudRoots roots = rootsOf(install);
    REQUIRE(legendsHudFilesFound(kLegendsN64HudStyle, roots));
    REQUIRE(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));
    // Every file either look needs is there, but this build has a painter for neither.
    CHECK(legendsHudStyles(roots).empty());

    // Neither opens, each saying why in one warning, and nothing is uploaded.
    test::FakeRenderDevice device;
    DarkLegacyHudPainter darkLegacy;
    for (const std::string_view id : std::array{kLegendsN64HudStyle, kLegendsDreamcastHudStyle}) {
        CAPTURE(id);
        const Opening opening = openingOf(id, roots, device, darkLegacy);
        CHECK(opening.painter == nullptr);
        REQUIRE(opening.warnings.size() == 1);
        CHECK(opening.warnings[0].starts_with(std::format("HUD: the {} look could not be", id)));
        CHECK(opening.warnings[0].find("no painter") != std::string::npos);
    }
    // Nor do Dark Legacy's own look or an id no look has.
    for (const std::string_view id :
         std::array<std::string_view, 2>{kDarkLegacyHudStyle, "future"}) {
        CAPTURE(id);
        const Opening opening = openingOf(id, roots, device, darkLegacy);
        CHECK(opening.painter == nullptr);
        REQUIRE(opening.warnings.size() == 1);
        CHECK(opening.warnings[0].find("no Legends look has that id") != std::string::npos);
    }
    CHECK(device.texturesCreated == 0);
}

TEST_CASE("a look whose files went after the probe opens nothing", "[legends][hud-style]") {
    const auto install = test::scratchDirectory("legends-hud-gone");
    n64At(install / "n64/extracted");
    dreamcastAt(install / "dreamcast/extracted");
    const LegendsHudRoots roots = rootsOf(install);
    REQUIRE(legendsHudFilesFound(kLegendsN64HudStyle, roots));
    REQUIRE(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));

    // A font of each goes between the probe and the scene's opening.
    REQUIRE(std::filesystem::remove(install / "n64/extracted/Font/numfont.FNT"));
    REQUIRE(std::filesystem::remove(install / "dreamcast/extracted/native/fonts/font32.fnt"));
    test::FakeRenderDevice device;
    DarkLegacyHudPainter darkLegacy;
    const Opening n64 = openingOf(kLegendsN64HudStyle, roots, device, darkLegacy);
    CHECK(n64.painter == nullptr);
    REQUIRE(n64.warnings.size() == 1);
    CHECK(n64.warnings[0].starts_with("HUD: the n64 look could not be opened ("));
    CHECK(n64.warnings[0].find("Font/numfont.FNT") != std::string::npos);
    const Opening dreamcast = openingOf(kLegendsDreamcastHudStyle, roots, device, darkLegacy);
    CHECK(dreamcast.painter == nullptr);
    REQUIRE(dreamcast.warnings.size() == 1);
    CHECK(dreamcast.warnings[0].starts_with("HUD: the dreamcast look could not be opened ("));
    CHECK(dreamcast.warnings[0].find("fonts/font32.fnt") != std::string::npos);
    CHECK(device.texturesCreated == 0);

    // A campaign gone whole is said to be gone.
    std::filesystem::remove_all(install / "n64");
    const Opening gone = openingOf(kLegendsN64HudStyle, roots, device, darkLegacy);
    CHECK(gone.painter == nullptr);
    REQUIRE(gone.warnings.size() == 1);
    CHECK(gone.warnings[0].find("no extraction") != std::string::npos);
}
