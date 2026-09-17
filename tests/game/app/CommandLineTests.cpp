#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/app/Application.h"

#include "game/app/CommandLine.h"

namespace {

using namespace gdl;
using namespace gdl::game;

ApplicationDesc defaults() {
    ApplicationDesc desc;
    desc.window.title = "test";
    desc.assetDirectory = "default/assets";
    desc.vsync = true;
    desc.enableValidation = true;
    return desc;
}

TEST_CASE("no arguments keeps the defaults", "[game][commandline]") {
    const CommandLineResult result = parseCommandLine({}, defaults());
    REQUIRE(result.action == CommandLineAction::Run);
    REQUIRE(result.desc.assetDirectory == "default/assets");
    REQUIRE(result.desc.vsync);
    REQUIRE(result.desc.enableValidation);
    REQUIRE(result.desc.maxFrames == 0);
    REQUIRE(result.desc.window.title == "test");
}

TEST_CASE("options override the defaults", "[game][commandline]") {
    constexpr std::array<std::string_view, 6> kArgs{"--assets",        "W:/data",  "--no-vsync",
                                                    "--no-validation", "--frames", "42"};
    const CommandLineResult result = parseCommandLine(kArgs, defaults());
    REQUIRE(result.action == CommandLineAction::Run);
    REQUIRE(result.desc.assetDirectory == "W:/data");
    REQUIRE_FALSE(result.desc.vsync);
    REQUIRE_FALSE(result.desc.enableValidation);
    REQUIRE(result.desc.maxFrames == 42);
}

TEST_CASE("the validation flag forces the layer on", "[game][commandline]") {
    ApplicationDesc desc = defaults();
    desc.enableValidation = false;
    constexpr std::array<std::string_view, 1> kArgs{"--validation"};
    REQUIRE(parseCommandLine(kArgs, desc).desc.enableValidation);
}

TEST_CASE("the movie flag selects a single movie to play", "[game][commandline]") {
    constexpr std::array<std::string_view, 2> kArgs{"--movie", "opening"};
    const CommandLineResult result = parseCommandLine(kArgs, defaults());
    REQUIRE(result.action == CommandLineAction::Run);
    REQUIRE(result.options.playMovie == "opening");
    REQUIRE(parseCommandLine({}, defaults()).options.playMovie.empty());
    constexpr std::array<std::string_view, 1> kMissing{"--movie"};
    REQUIRE(parseCommandLine(kMissing, defaults()).action == CommandLineAction::Fail);
}

TEST_CASE("the data flag points at the configuration directory", "[game][commandline]") {
    constexpr std::array<std::string_view, 2> kArgs{"--data", "W:/conf"};
    const CommandLineResult result = parseCommandLine(kArgs, defaults());
    REQUIRE(result.action == CommandLineAction::Run);
    REQUIRE(result.options.dataDirectory == "W:/conf");
    constexpr std::array<std::string_view, 1> kMissing{"--data"};
    REQUIRE(parseCommandLine(kMissing, defaults()).action == CommandLineAction::Fail);
}

TEST_CASE("--help and -h request the usage text", "[game][commandline]") {
    constexpr std::array<std::string_view, 1> kLong{"--help"};
    constexpr std::array<std::string_view, 1> kShort{"-h"};
    REQUIRE(parseCommandLine(kLong, defaults()).action == CommandLineAction::ShowHelp);
    REQUIRE(parseCommandLine(kShort, defaults()).action == CommandLineAction::ShowHelp);
    REQUIRE(std::string_view(usageText()).starts_with("gauntlet"));
}

TEST_CASE("bad input fails with a message", "[game][commandline]") {
    SECTION("unknown flag") {
        constexpr std::array<std::string_view, 1> kArgs{"--bogus"};
        const CommandLineResult result = parseCommandLine(kArgs, defaults());
        REQUIRE(result.action == CommandLineAction::Fail);
        REQUIRE(result.message.contains("--bogus"));
    }
    SECTION("missing value") {
        constexpr std::array<std::string_view, 1> kArgs{"--assets"};
        REQUIRE(parseCommandLine(kArgs, defaults()).action == CommandLineAction::Fail);
    }
    SECTION("non-numeric frame count") {
        constexpr std::array<std::string_view, 2> kArgs{"--frames", "many"};
        const CommandLineResult result = parseCommandLine(kArgs, defaults());
        REQUIRE(result.action == CommandLineAction::Fail);
        REQUIRE(result.message.contains("many"));
    }
}

} // namespace
