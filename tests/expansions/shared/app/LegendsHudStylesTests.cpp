#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeDarkLegacyArt.h"
#include "FakeRenderDevice.h"
#include "LogCapture.h"
#include "TestSupport.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudFixture.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudPainter.h"
#include "expansions/n64/hud/LegendsN64HudFixture.h"
#include "expansions/n64/hud/LegendsN64HudPainter.h"
#include "expansions/shared/app/LegendsHudStyles.h"
#include "expansions/shared/characters/LegendsProgress.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "game/characters/ClassData.h"
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

/** The synthetic N64 extraction of the look's painter tests, installed in a fresh `install` as
 * the launcher installs it, with a map for legendsMaps to find it by. */
struct InstalledN64 {
    test::LegendsN64HudFixture files;
    LegendsHudRoots roots;

    explicit InstalledN64(std::string_view install)
        : files(std::format("{}/n64/extracted", install)),
          roots(rootsOf(files.root().parent_path().parent_path())) {
        fixture(files.root() / "levels/tower2/worlds.wad");
        fixture(files.root() / "levels/tower2/objects.wad");
    }
};

/** The synthetic Dreamcast extraction of the look's painter tests, installed in a fresh `install`
 * as the launcher installs it, its native files under native, with a map for legendsMaps to find
 * it by. */
struct InstalledDreamcast {
    test::LegendsDreamcastHudFixture files;
    LegendsHudRoots roots;

    explicit InstalledDreamcast(std::string_view install)
        : files(std::format("{}/dreamcast/extracted/native", install)),
          roots(rootsOf(files.root().parent_path().parent_path().parent_path())) {
        fixture(files.root() / "levels/levelt1/worlds.rom");
        fixture(files.root() / "levels/levelt1/objects.rom");
    }
};

/** The N64 look's offer as the HUD page is given it. */
HudStyleOffer n64Offer(bool usable) {
    return {.id = std::string(kLegendsN64HudStyle),
            .label = std::string(kLegendsN64HudLabel),
            .usable = usable};
}

/** The Dreamcast look's offer as the HUD page is given it. */
HudStyleOffer dreamcastOffer(bool usable) {
    return {.id = std::string(kLegendsDreamcastHudStyle),
            .label = std::string(kLegendsDreamcastHudLabel),
            .usable = usable};
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
                  DarkLegacyHudArt& darkLegacy) {
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

TEST_CASE("the N64 look's probe looks for just the files its painter cannot open without",
          "[legends][hud-style]") {
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    const std::array party{HudMember{.seat = 0, .classIndex = kWarriorClass, .color = 0}};
    const std::span<const std::string_view> required = legendsHudRequiredFiles(kLegendsN64HudStyle);
    REQUIRE_FALSE(required.empty());

    // With the probe's files alone, the rest of the extraction gone, the look opens.
    const test::LegendsN64HudFixture alone("legends-hud-n64-required");
    std::vector<std::filesystem::path> others;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(alone.root())) {
        const std::string name =
            std::filesystem::relative(entry.path(), alone.root()).generic_string();
        if (entry.is_regular_file() &&
            std::ranges::find(required, std::string_view{name}) == required.end()) {
            others.push_back(entry.path());
        }
    }
    REQUIRE_FALSE(others.empty());
    for (const std::filesystem::path& other : others) {
        std::filesystem::remove(other);
    }
    LegendsN64HudPainter look;
    CHECK(look.load(device, alone.root(), art, party));

    // Without any one of them, it does not.
    for (const std::string_view name : required) {
        CAPTURE(name);
        const test::LegendsN64HudFixture files("legends-hud-n64-lacking");
        REQUIRE(std::filesystem::remove(files.root() / name));
        LegendsN64HudPainter lacking;
        CHECK_FALSE(lacking.load(device, files.root(), art, party));
    }
}

TEST_CASE("the Dreamcast look's probe looks for just the files its painter cannot open without",
          "[legends][hud-style]") {
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    const std::array party{HudMember{.seat = 0, .classIndex = kWarriorClass, .color = 0}};
    const std::span<const std::string_view> required =
        legendsHudRequiredFiles(kLegendsDreamcastHudStyle);
    REQUIRE_FALSE(required.empty());

    // With the probe's files alone, the rest of the extraction gone, the look opens.
    const test::LegendsDreamcastHudFixture alone("legends-hud-dreamcast-required");
    std::vector<std::filesystem::path> others;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(alone.root())) {
        const std::string name =
            std::filesystem::relative(entry.path(), alone.root()).generic_string();
        if (entry.is_regular_file() &&
            std::ranges::find(required, std::string_view{name}) == required.end()) {
            others.push_back(entry.path());
        }
    }
    REQUIRE_FALSE(others.empty());
    for (const std::filesystem::path& other : others) {
        std::filesystem::remove(other);
    }
    LegendsDreamcastHudPainter look;
    CHECK(look.load(device, alone.root(), art, party));

    // Without any one of them, it does not.
    for (const std::string_view name : required) {
        CAPTURE(name);
        const test::LegendsDreamcastHudFixture files("legends-hud-dreamcast-lacking");
        REQUIRE(std::filesystem::remove(files.root() / name));
        LegendsDreamcastHudPainter lacking;
        CHECK_FALSE(lacking.load(device, files.root(), art, party));
    }
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

TEST_CASE("every Legends look is offered, in the HUD page's order", "[legends][hud-style]") {
    const auto install = test::scratchDirectory("legends-hud-built");
    n64At(install / "n64/extracted");
    dreamcastAt(install / "dreamcast/extracted");
    const LegendsHudRoots roots = rootsOf(install);
    REQUIRE(legendsHudFilesFound(kLegendsN64HudStyle, roots));
    REQUIRE(legendsHudFilesFound(kLegendsDreamcastHudStyle, roots));
    // Both looks' files are there: the N64's, then the Dreamcast's.
    CHECK(legendsHudStyles(roots) ==
          std::vector<HudStyleOffer>{n64Offer(true), dreamcastOffer(true)});

    // Dark Legacy's own look and an id no look has open nothing, saying why in one warning, and
    // nothing is uploaded.
    test::FakeRenderDevice device;
    DarkLegacyHudPainter darkLegacy;
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

TEST_CASE("the N64 look is offered once its files are found", "[legends][hud-style]") {
    const InstalledN64 n64("legends-hud-n64-offered");
    // Offered, and usable: the HUD page lists it under its own name. The Dreamcast's, not
    // installed here, is listed but cannot be chosen.
    CHECK(legendsHudStyles(n64.roots) ==
          std::vector<HudStyleOffer>{n64Offer(true), dreamcastOffer(false)});

    // Chosen, it opens the N64's painter, which takes its art at once with nothing to warn of.
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    const Opening opening = openingOf(kLegendsN64HudStyle, n64.roots, device, art);
    REQUIRE(opening.painter != nullptr);
    CHECK(opening.painter->id() == kLegendsN64HudStyle);
    CHECK(dynamic_cast<const LegendsN64HudPainter*>(opening.painter.get()) != nullptr);
    CHECK(opening.warnings.empty());
    CHECK(device.texturesCreated > 0);
    CHECK_FALSE(art.lookups.empty());

    // Without a file it cannot be drawn without, it is still listed, but cannot be chosen.
    REQUIRE(std::filesystem::remove(n64.files.root() / LegendsN64HudPainter::kNumberFontFile));
    CHECK(legendsHudStyles(n64.roots) ==
          std::vector<HudStyleOffer>{n64Offer(false), dreamcastOffer(false)});
}

TEST_CASE("the Dreamcast look is offered once its files are found", "[legends][hud-style]") {
    const InstalledDreamcast dreamcast("legends-hud-dreamcast-offered");
    // Offered, and usable: the HUD page lists it under its own name, after the N64's, which is
    // not installed here.
    CHECK(legendsHudStyles(dreamcast.roots) ==
          std::vector<HudStyleOffer>{n64Offer(false), dreamcastOffer(true)});

    // Chosen, it opens the Dreamcast's painter, which takes its art at once with nothing to warn
    // of.
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    const Opening opening = openingOf(kLegendsDreamcastHudStyle, dreamcast.roots, device, art);
    REQUIRE(opening.painter != nullptr);
    CHECK(opening.painter->id() == kLegendsDreamcastHudStyle);
    CHECK(dynamic_cast<const LegendsDreamcastHudPainter*>(opening.painter.get()) != nullptr);
    CHECK(opening.warnings.empty());
    CHECK(device.texturesCreated > 0);
    CHECK_FALSE(art.lookups.empty());

    // Without a file it cannot be drawn without, it is still listed, but cannot be chosen.
    REQUIRE(std::filesystem::remove(dreamcast.files.root() /
                                    LegendsDreamcastHudPainter::kLargeFontFile));
    CHECK(legendsHudStyles(dreamcast.roots) ==
          std::vector<HudStyleOffer>{n64Offer(false), dreamcastOffer(false)});
}

TEST_CASE("a Dreamcast look whose files are there but cannot be read opens nothing, and says "
          "which",
          "[legends][hud-style]") {
    std::string_view cut;
    SECTION("its static set's texels cut short") {
        cut = LegendsDreamcastHudPainter::kStaticTextures;
    }
    SECTION("its static set's table cut short") {
        cut = LegendsDreamcastHudPainter::kStaticObjects;
    }
    SECTION("its score font cut short") {
        cut = LegendsDreamcastHudPainter::kScoreFontFile;
    }
    CAPTURE(cut);
    const InstalledDreamcast dreamcast("legends-hud-dreamcast-unreadable");
    dreamcast.files.truncate(cut);
    // Cut short, not removed: the probe finds it, and the look is offered.
    REQUIRE(legendsHudFilesFound(kLegendsDreamcastHudStyle, dreamcast.roots));
    CHECK(legendsHudStyles(dreamcast.roots) ==
          std::vector<HudStyleOffer>{n64Offer(false), dreamcastOffer(true)});
    // The scene's opening reads it, and draws Dark Legacy's look: no exception reaches it, and
    // one warning names the file.
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    Opening opening;
    REQUIRE_NOTHROW(opening = openingOf(kLegendsDreamcastHudStyle, dreamcast.roots, device, art));
    CHECK(opening.painter == nullptr);
    REQUIRE(opening.warnings.size() == 1);
    CHECK(opening.warnings[0].starts_with("HUD: the dreamcast look could not be opened ("));
    CHECK(opening.warnings[0].find(cut) != std::string::npos);
}

TEST_CASE("an N64 look whose files are there but cannot be read opens nothing, and says which",
          "[legends][hud-style]") {
    std::string_view cut;
    SECTION("its HUD bank cut short") {
        cut = LegendsN64HudPainter::kHudBank;
    }
    SECTION("its health font cut short") {
        cut = LegendsN64HudPainter::kNumberFontFile;
    }
    CAPTURE(cut);
    const InstalledN64 n64("legends-hud-n64-unreadable");
    n64.files.truncate(cut);
    // Cut short, not removed: the probe finds it, and the look is offered.
    REQUIRE(legendsHudFilesFound(kLegendsN64HudStyle, n64.roots));
    CHECK(legendsHudStyles(n64.roots) ==
          std::vector<HudStyleOffer>{n64Offer(true), dreamcastOffer(false)});
    // The scene's opening reads it, and draws Dark Legacy's look: no exception reaches it, and
    // one warning names the file.
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    Opening opening;
    REQUIRE_NOTHROW(opening = openingOf(kLegendsN64HudStyle, n64.roots, device, art));
    CHECK(opening.painter == nullptr);
    REQUIRE(opening.warnings.size() == 1);
    CHECK(opening.warnings[0].starts_with("HUD: the n64 look could not be opened ("));
    CHECK(opening.warnings[0].find(cut) != std::string::npos);
}
