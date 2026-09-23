#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/AudioRom.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using test::ByteWriter;

std::vector<std::uint8_t> sampleRom() {
    ByteWriter w;
    w.putU32(1).putU32(2).putU32(3).putU32(24).putU32(24).putU32(24 + 2 * 44);
    w.putText("common").putZeros(10).putText("COMMON").putZeros(10);
    w.putU32(1000).putU16(2).putU16(0).putU16(0).putU16(0);
    w.putText("select").putZeros(10).putText("SELECT").putZeros(10);
    w.putU32(2000).putU16(1).putU16(2).putU16(0).putU16(0);
    w.putText("S_WARN").putZeros(10).putU32(0).putU32(0x3F000000).putU32(0);
    w.putText("S_OPTMENUSEL").putZeros(4).putU32(1).putU32(0x3F800000).putU32(0);
    w.putText("S_SELECTMUS").putZeros(5).putU32(0x10000).putU32(0xBF800000).putU32(0);
    return w.bytes();
}

TEST_CASE("the audio directory lists banks and their sounds", "[formats][audio]") {
    const AudioRom rom = AudioRom::parse(sampleRom());
    REQUIRE(rom.banks.size() == 2);
    REQUIRE(rom.sounds.size() == 3);
    REQUIRE(rom.banks[0].file == "common");
    REQUIRE(rom.banks[0].name == "COMMON");
    REQUIRE(rom.banks[0].dataSize == 1000);
    REQUIRE(rom.banks[0].soundCount == 2);
    REQUIRE(rom.banks[1].firstSound == 2);
    REQUIRE(rom.sounds[1].name == "S_OPTMENUSEL");
    REQUIRE(rom.sounds[1].id == 1);
    REQUIRE(rom.sounds[1].duration == 1.0f);
    REQUIRE(rom.sounds[2].id == 0x10000);
    REQUIRE(rom.sounds[2].duration < 0.0f);
    REQUIRE(rom.findBank("select") == 1U);
    REQUIRE(rom.findSound("S_WARN") == 0U);
    REQUIRE_FALSE(rom.findSound("S_MISSING").has_value());
}

TEST_CASE("damaged audio directories are rejected", "[formats][audio]") {
    REQUIRE_THROWS_AS(AudioRom::parse(std::vector<std::uint8_t>(8, 0)), FormatError);
    std::vector<std::uint8_t> bad = sampleRom();
    bad[24 + 44 + 38] = 9; // second bank's first sound past the table
    REQUIRE_THROWS_AS(AudioRom::parse(bad), FormatError);
}

TEST_CASE("the shipped directory names the menu sounds", "[formats][audio][assets]") {
    const auto file = test::assetOrSkip("AUDIO/AUDATPS2.ROM");
    const AudioRom rom = AudioRom::parse(readFile(file));
    REQUIRE(rom.banks.size() == 60);
    REQUIRE(rom.sounds.size() == 2228);
    REQUIRE(rom.banks[0].name == "COMMON");
    REQUIRE(rom.findBank("SELECT") == 12U);
    REQUIRE(rom.banks[12].firstSound == 599);
    const auto select = rom.findSound("S_OPTMENUSEL");
    REQUIRE(select.has_value());
    REQUIRE(rom.sounds[*select].id == 17);
    const auto music = rom.findSound("S_SELECTMUS");
    REQUIRE(music.has_value());
    REQUIRE(rom.sounds[*music].id == 0xC0000);
    REQUIRE(rom.sounds[*music].duration < 0.0f);
}

} // namespace
