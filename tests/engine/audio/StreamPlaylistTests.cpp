#include <memory>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/StreamPlaylist.h"
#include "engine/core/Types.h"

namespace {
using namespace gdl;
class Part final : public StreamSource {
public:
    explicit Part(f32 value, u32 rate = 48000) : m_value(value), m_rate(rate) {}
    AudioStreamDesc desc() const override { return {m_rate, 1}; }
    bool read(std::vector<f32>& out, usize /*frames*/) override {
        if (m_read || m_value == 0) {
            return false;
        }
        m_read = true;
        out.push_back(m_value);
        return true;
    }
    void rewind() override { m_read = false; }

private:
    f32 m_value;
    u32 m_rate;
    bool m_read = false;
};

TEST_CASE("music parts play in order and only the final part repeats", "[audio][stream]") {
    std::vector<std::unique_ptr<StreamSource>> parts;
    parts.push_back(std::make_unique<Part>(1));
    parts.push_back(std::make_unique<Part>(2));
    parts.push_back(std::make_unique<Part>(3));
    StreamPlaylist playlist(std::move(parts));
    std::vector<f32> out;
    for (s32 i = 0; i < 5; ++i) {
        REQUIRE(playlist.read(out, 1));
    }
    REQUIRE(out == std::vector<f32>{1, 2, 3, 3, 3});
    playlist.rewind();
    out.clear();
    REQUIRE(playlist.read(out, 1));
    REQUIRE(out == std::vector<f32>{1});
}

TEST_CASE("music playlists reject incompatible formats and terminate an empty loop",
          "[audio][stream]") {
    SECTION("empty playlist") {
        std::vector<std::unique_ptr<StreamSource>> parts;
        REQUIRE_THROWS_AS(StreamPlaylist(std::move(parts)), std::invalid_argument);
    }
    SECTION("format change") {
        std::vector<std::unique_ptr<StreamSource>> parts;
        parts.push_back(std::make_unique<Part>(1));
        parts.push_back(std::make_unique<Part>(2, 22050));
        REQUIRE_THROWS_AS(StreamPlaylist(std::move(parts)), std::invalid_argument);
    }
    SECTION("empty final part") {
        std::vector<std::unique_ptr<StreamSource>> parts;
        parts.push_back(std::make_unique<Part>(1));
        parts.push_back(std::make_unique<Part>(0));
        StreamPlaylist playlist(std::move(parts));
        std::vector<f32> out;
        REQUIRE(playlist.read(out, 1));
        REQUIRE_FALSE(playlist.read(out, 1));
        REQUIRE(out == std::vector<f32>{1});
    }
}
} // namespace
