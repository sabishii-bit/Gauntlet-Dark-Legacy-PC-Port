#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

TEST_CASE("files round-trip through writeFile and readFile", "[io][file]") {
    const auto dir = test::scratchDirectory("file-roundtrip");
    const std::vector<std::uint8_t> payload{1, 2, 3, 250, 251, 252};
    writeFile(dir / "data.bin", payload);
    REQUIRE(readFile(dir / "data.bin") == payload);
}

TEST_CASE("FileStream seeks and reads", "[io][file]") {
    const auto dir = test::scratchDirectory("file-stream");
    std::vector<std::uint8_t> payload(100);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i);
    }
    writeFile(dir / "stream.bin", payload);

    FileStream stream(dir / "stream.bin");
    REQUIRE(stream.size() == 100);
    REQUIRE(stream.position() == 0);

    std::array<std::uint8_t, 4> head{};
    REQUIRE(stream.read(head) == 4);
    REQUIRE(head == std::array<std::uint8_t, 4>{0, 1, 2, 3});
    REQUIRE(stream.position() == 4);

    stream.seek(96);
    const auto tail = stream.readExact(4);
    REQUIRE(tail == std::vector<std::uint8_t>{96, 97, 98, 99});
    REQUIRE(stream.position() == 100);

    std::array<std::uint8_t, 4> beyond{};
    REQUIRE(stream.read(beyond) == 0);
    REQUIRE_THROWS_AS(stream.readExact(1), FileError);
    REQUIRE_THROWS_AS(stream.seek(101), FileError);
}

TEST_CASE("missing files throw FileError", "[io][file]") {
    const auto dir = test::scratchDirectory("file-missing");
    REQUIRE_THROWS_AS(readFile(dir / "nope.bin"), FileError);
    REQUIRE_THROWS_AS(FileStream(dir / "nope.bin"), FileError);
}

TEST_CASE("text files read back as strings", "[io][file]") {
    const auto dir = test::scratchDirectory("file-text");
    writeTextFile(dir / "note.txt", "hello\nworld");
    REQUIRE(readTextFile(dir / "note.txt") == "hello\nworld");
    REQUIRE_THROWS_AS(readTextFile(dir / "missing.txt"), FileError);
}

} // namespace
