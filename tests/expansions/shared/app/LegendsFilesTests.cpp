#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/shared/app/LegendsFiles.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A file at `path` holding `bytes`, its folders made. */
void fixture(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    writeTextFile(path, bytes);
}
} // namespace

TEST_CASE("a file counts only when it is there and not empty", "[legends][legends-files]") {
    const auto root = test::scratchDirectory("legends-files");
    fixture(root / "Font/numfont.FNT", "native fixture");
    fixture(root / "Font/glfm3.fnt", "");
    std::filesystem::create_directories(root / "hud/objects.wad");
    const AssetLocator files{root};

    CHECK(nonemptyFile(files, "Font/numfont.FNT"));
    // Found whatever its letter case, as every extracted file is.
    CHECK(nonemptyFile(files, "font/NUMFONT.fnt"));
    // An extraction cut short leaves a file holding nothing.
    CHECK_FALSE(nonemptyFile(files, "Font/glfm3.fnt"));
    CHECK_FALSE(nonemptyFile(files, "Font/4x6font.fnt"));
    CHECK_FALSE(nonemptyFile(files, "Fonts/numfont.FNT"));
    // A folder by the file's name is no file.
    CHECK_FALSE(nonemptyFile(files, "hud/objects.wad"));
    // Under a root that is not there, nothing counts.
    CHECK_FALSE(nonemptyFile(AssetLocator{root / "missing"}, "Font/numfont.FNT"));
}
