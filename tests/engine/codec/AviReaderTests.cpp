#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/codec/AviReader.h"
#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;
using test::ByteWriter;

std::vector<u8> chunk(std::string_view id, std::span<const u8> body) {
    ByteWriter w;
    w.putFourcc(id).putU32(static_cast<u32>(body.size())).putBytes(body);
    if (body.size() % 2 != 0) {
        w.putU8(0);
    }
    return w.bytes();
}

std::vector<u8> list(std::string_view type, std::span<const u8> body) {
    ByteWriter w;
    w.putFourcc("LIST").putU32(static_cast<u32>(body.size() + 4)).putFourcc(type).putBytes(body);
    return w.bytes();
}

std::vector<u8> videoStreamList() {
    ByteWriter strh;
    strh.putFourcc("vids").putFourcc("MVDV").putU32(0).putU16(0).putU16(0).putU32(0);
    strh.putU32(1).putU32(30).putU32(0).putU32(3).putU32(0).putU32(0).putU32(0).putZeros(8);
    ByteWriter strf;
    strf.putU32(40).putU32(8).putS32(-4).putU16(1).putU16(24).putFourcc("MVDV").putU32(96);
    strf.putZeros(16);
    ByteWriter body;
    body.putBytes(chunk("strh", strh.bytes())).putBytes(chunk("strf", strf.bytes()));
    return list("strl", body.bytes());
}

std::vector<u8> audioStreamList() {
    ByteWriter strh;
    strh.putFourcc("auds").putU32(0).putU32(0).putU16(0).putU16(0).putU32(0);
    strh.putU32(1).putU32(8000).putU32(0).putU32(16).putU32(0).putU32(0).putU32(1).putZeros(8);
    ByteWriter strf;
    strf.putU16(1).putU16(1).putU32(8000).putU32(8000).putU16(1).putU16(8);
    ByteWriter body;
    body.putBytes(chunk("strh", strh.bytes())).putBytes(chunk("strf", strf.bytes()));
    return list("strl", body.bytes());
}

std::filesystem::path writeSampleAvi() {
    ByteWriter avih;
    avih.putU32(33333).putU32(0).putU32(0).putU32(0).putU32(3).putU32(0).putU32(2).putU32(0);
    avih.putU32(8).putU32(4).putZeros(16);

    ByteWriter header;
    header.putBytes(chunk("avih", avih.bytes()));
    header.putBytes(videoStreamList());
    header.putBytes(audioStreamList());

    const std::vector<u8> audio{128, 129, 130, 131};
    const std::vector<u8> inner{7, 8};
    ByteWriter movie;
    movie.putBytes(chunk("00dc", std::vector<u8>{'a', 'b', 'c'}));
    movie.putBytes(chunk("01wb", audio));
    movie.putBytes(list("rec ", chunk("00db", inner)));
    movie.putBytes(chunk("JUNK", std::vector<u8>{0, 0}));
    movie.putBytes(chunk("00dc", std::vector<u8>{'z'}));

    ByteWriter riffBody;
    riffBody.putFourcc("AVI ");
    riffBody.putBytes(list("hdrl", header.bytes()));
    riffBody.putBytes(chunk("JUNK", std::vector<u8>{0, 0, 0, 0}));
    riffBody.putBytes(list("movi", movie.bytes()));
    riffBody.putBytes(chunk("idx1", std::vector<u8>{1, 2, 3, 4}));

    ByteWriter file;
    file.putFourcc("RIFF").putU32(static_cast<u32>(riffBody.size())).putBytes(riffBody.bytes());

    const auto dir = test::scratchDirectory("avi-reader");
    writeFile(dir / "sample.avi", file.bytes());
    return dir / "sample.avi";
}

TEST_CASE("the AVI header and stream formats are parsed", "[codec][avi]") {
    const AviReader reader(writeSampleAvi());
    const AviHeader& header = reader.header();
    REQUIRE(header.microSecondsPerFrame == 33333);
    REQUIRE(header.totalFrames == 3);
    REQUIRE(header.width == 8);
    REQUIRE(header.height == 4);
    REQUIRE(header.streams.size() == 2);

    const AviStream& video = header.streams[0];
    REQUIRE(video.type == fourcc("vids"));
    REQUIRE(video.handler == fourcc("MVDV"));
    REQUIRE(video.scale == 1);
    REQUIRE(video.rate == 30);
    REQUIRE(video.length == 3);
    REQUIRE(video.video.has_value());
    REQUIRE(video.video->width == 8);
    REQUIRE(video.video->height == -4);
    REQUIRE(video.video->bitCount == 24);
    REQUIRE(video.video->compression == fourcc("MVDV"));
    REQUIRE_FALSE(video.audio.has_value());

    const AviStream& audio = header.streams[1];
    REQUIRE(audio.type == fourcc("auds"));
    REQUIRE(audio.rate == 8000);
    REQUIRE(audio.sampleSize == 1);
    REQUIRE(audio.audio.has_value());
    REQUIRE(audio.audio->formatTag == 1);
    REQUIRE(audio.audio->channels == 1);
    REQUIRE(audio.audio->samplesPerSecond == 8000);
    REQUIRE(audio.audio->bitsPerSample == 8);
}

TEST_CASE("movie chunks stream in order, descending into rec lists and skipping junk",
          "[codec][avi]") {
    AviReader reader(writeSampleAvi());

    auto first = reader.next();
    REQUIRE(first.has_value());
    REQUIRE(first->kind == AviChunkKind::Video);
    REQUIRE(first->stream == 0);
    REQUIRE(first->data == std::vector<u8>{'a', 'b', 'c'});

    auto second = reader.next();
    REQUIRE(second.has_value());
    REQUIRE(second->kind == AviChunkKind::Audio);
    REQUIRE(second->stream == 1);
    REQUIRE(second->data.size() == 4);

    auto third = reader.next();
    REQUIRE(third.has_value());
    REQUIRE(third->kind == AviChunkKind::Video);
    REQUIRE(third->data == std::vector<u8>{7, 8});

    auto fourth = reader.next();
    REQUIRE(fourth.has_value());
    REQUIRE(fourth->data == std::vector<u8>{'z'});

    REQUIRE_FALSE(reader.next().has_value());
    REQUIRE_FALSE(reader.next().has_value());
}

TEST_CASE("files that are not AVI are rejected", "[codec][avi]") {
    const auto dir = test::scratchDirectory("avi-bad");
    writeFile(dir / "bad.avi", std::vector<u8>{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'});
    REQUIRE_THROWS_AS(AviReader(dir / "bad.avi"), FormatError);
    REQUIRE_THROWS_AS(AviReader(dir / "missing.avi"), FileError);
}

} // namespace
