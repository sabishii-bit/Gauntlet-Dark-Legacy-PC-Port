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

TEST_CASE("a continuation takes the playlist over where the part playing ends", "[audio][stream]") {
    std::vector<std::unique_ptr<StreamSource>> parts;
    parts.push_back(std::make_unique<Part>(1));
    parts.push_back(std::make_unique<Part>(2));
    StreamPlaylist playlist(std::move(parts));
    std::vector<f32> out;
    REQUIRE(playlist.read(out, 1));
    REQUIRE_FALSE(playlist.following());
    std::vector<std::unique_ptr<StreamSource>> next;
    next.push_back(std::make_unique<Part>(7));
    next.push_back(std::make_unique<Part>(8));
    playlist.follow(std::move(next));
    REQUIRE(playlist.following());
    REQUIRE(playlist.followed() == 0);
    // The first part is over: the continuation plays from its start, then repeats its last.
    for (s32 i = 0; i < 3; ++i) {
        REQUIRE(playlist.read(out, 1));
    }
    REQUIRE(out == std::vector<f32>{1, 7, 8, 8});
    REQUIRE(playlist.followed() == 1);
    REQUIRE(playlist.part() == 1);
    REQUIRE_FALSE(playlist.following());
    // A continuation can be replaced or withdrawn while it waits; at the loop point of the
    // last part one that stands takes over.
    std::vector<std::unique_ptr<StreamSource>> dropped;
    dropped.push_back(std::make_unique<Part>(9));
    playlist.follow(std::move(dropped));
    playlist.follow({});
    REQUIRE_FALSE(playlist.following());
    out.clear();
    REQUIRE(playlist.read(out, 1));
    REQUIRE(out == std::vector<f32>{8});
    std::vector<std::unique_ptr<StreamSource>> loop;
    loop.push_back(std::make_unique<Part>(5));
    playlist.follow(std::move(loop));
    REQUIRE(playlist.read(out, 1));
    REQUIRE(out == std::vector<f32>{8, 5});
    REQUIRE(playlist.followed() == 2);
    // A continuation of another format is refused.
    std::vector<std::unique_ptr<StreamSource>> other;
    other.push_back(std::make_unique<Part>(1, 22050));
    REQUIRE_THROWS_AS(playlist.follow(std::move(other)), std::invalid_argument);
}
} // namespace
