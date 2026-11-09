#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include <amuse/AudioGroup.hpp>
#include <amuse/Engine.hpp>
#include <amuse/IBackendSubmix.hpp>
#include <amuse/IBackendVoice.hpp>
#include <amuse/IBackendVoiceAllocator.hpp>
#include <amuse/Sequencer.hpp>
#include <amuse/SongState.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct SilentMix final : amuse::IBackendSubmix {
    void setSendLevel(amuse::IBackendSubmix* /*submix*/, float /*level*/, bool /*slew*/) override {}
    double getSampleRate() const override { return 32000; }
    amuse::SubmixFormat getSampleFormat() const override { return amuse::SubmixFormat::Float; }
};
// These arrangements contain only program changes. No samples or ROM are needed
// to test the actual sequencer's region/loop timing rather than PCM loudness.
struct TimingBackend final : amuse::IBackendVoiceAllocator {
    std::unique_ptr<amuse::IBackendVoice> allocateVoice(amuse::Voice& /*clientVox*/,
                                                        double /*sampleRate*/,
                                                        bool /*dynamicPitch*/) override {
        FAIL("Timing fixture must not allocate a voice");
        return {};
    }
    std::unique_ptr<amuse::IBackendSubmix>
    allocateSubmix(amuse::Submix& /*clientSmx*/, bool /*mainOut*/, int /*busId*/) override {
        return std::make_unique<SilentMix>();
    }
    std::vector<std::pair<std::string, std::string>> enumerateMIDIDevices() override { return {}; }
    std::unique_ptr<amuse::IMIDIReader> allocateMIDIReader(amuse::Engine& /*engine*/) override {
        return {};
    }
    amuse::AudioChannelSet getAvailableSet() override { return amuse::AudioChannelSet::Stereo; }
    void setVolume(float /*vol*/) override {}
    void setCallbackInterface(amuse::Engine* /*engine*/) override {}
};

std::array<unsigned char, 512> arrangement(bool tempoChanges) {
    std::array<unsigned char, 512> bytes{};
    const auto word = [&](size_t at, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) {
            bytes[at + i] = static_cast<unsigned char>(value >> (24 - i * 8));
        }
    };
    word(0, 0x60);  // 64 track offsets
    word(4, 0x160); // region offsets
    word(8, 0x164); // 64 channel IDs
    word(16, 60);   // 384 ticks/second
    word(20, 384);  // loop clock differs from the reused region, like cathy2a.song
    word(0x60, 0x1a4);
    word(0x160, 0x1c0);
    bytes[0x1a8] = bytes[0x1a9] = 0xff;
    word(0x1b0, 1152); // end at three seconds; loop to track region zero
    bytes[0x1b8] = 0xff;
    bytes[0x1b9] = 0xfe;
    word(0x1c0, 8);
    // Delta 0: program1. Delta384: program2. Delta384: end of pattern.
    constexpr std::array<unsigned char, 12> kEvents{0,    0, 0x81, 0,    1,    0x80,
                                                    0x82, 0, 1,    0x80, 0xff, 0xff};
    std::copy(kEvents.begin(), kEvents.end(), bytes.begin() + 0x1cc);
    if (tempoChanges) {
        word(12, 0x1e0);
        word(0x1e0, 0);
        word(0x1e4, 60);
        word(0x1e8, 384);
        word(0x1ec, 120);
        word(0x1f0, 0xffffffff);
    }
    return bytes;
}

std::array<unsigned char, 768> legacyArrangement(bool tempoChanges) {
    std::array<unsigned char, 768> bytes{};
    const auto word = [&](size_t at, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) {
            bytes[at + i] = static_cast<unsigned char>(value >> (24 - i * 8));
        }
    };
    word(0, 0x60);
    word(4, 0x160);
    word(8, 0x168);
    word(16, 60);
    word(20, 384);
    word(0x60, 0x200);
    word(0x64, 0x218);
    word(0x160, 0x240);
    word(0x164, 0x264);
    bytes[0x169] = 1;
    bytes[0x204] = bytes[0x205] = 0xff;
    word(0x20c, 1152);
    bytes[0x214] = 0xff;
    bytes[0x215] = 0xfe;
    bytes[0x21c] = bytes[0x21d] = 0xff;
    bytes[0x221] = 1;
    word(0x224, 1152);
    bytes[0x22c] = bytes[0x22d] = 0xff;
    word(0x240, 8);
    // Legacy MusyX stores absolute pattern times, followed by
    // length/key/velocity. Two adjacent regions force DetectVersion to inspect
    // a real payload rather than defaulting to revision1 on a one-region file.
    bytes[0x252] = 0x81;
    word(0x254, 384);
    bytes[0x25a] = 0x82;
    word(0x25c, 768);
    bytes[0x262] = bytes[0x263] = 0xff;
    word(0x264, 8);
    bytes[0x276] = bytes[0x277] = 0xff;
    if (tempoChanges) {
        word(12, 0x280);
        word(0x280, 0);
        word(0x284, 60);
        word(0x288, 384);
        word(0x28c, 120);
        word(0x290, 0xffffffff);
    }
    // The upstream heuristic first attempts the other event format and reads
    // until a terminator before checking the adjacent-region boundary. Keep
    // that deliberately unsuccessful trial inside this synthetic allocation.
    std::fill(bytes.begin() + 0x2e0, bytes.end(), 0xff);
    return bytes;
}
} // namespace

TEST_CASE("N64 music restarts pattern clocks while preserving the arrangement loop time",
          "[legends][audio][song-timing]") {
    const bool legacy = GENERATE(false, true);
    TimingBackend backend;
    amuse::AudioGroup group;
    const auto songs = amuse::MakeObj<amuse::SongGroupIndex>();
    songs->m_normPages.emplace(1, amuse::SongGroupIndex::PageEntry{});
    songs->m_normPages.emplace(2, amuse::SongGroupIndex::PageEntry{});
    group.getProj().songGroups().emplace(amuse::GroupId(0), songs);
    amuse::Engine engine(backend);
    // amuse entities must be ended by their engine (debug builds assert it), so the engine
    // makes this sequencer; it plays no arrangement itself, the song below drives it.
    const auto seq = engine.seqPlay(&group, amuse::GroupId(0), amuse::SongId(0), nullptr, true, {});
    REQUIRE(seq);
    bool tempoChanges = false;
    double dt = .005;
    SECTION("constant tempo") {}
    SECTION("loop resumes after earlier tempo changes") {
        tempoChanges = true;
    }
    SECTION("sub-tick updates do not apply future tempo changes") {
        tempoChanges = true;
        dt = .0005;
    }
    const auto revision = arrangement(tempoChanges);
    const auto original = legacyArrangement(tempoChanges);
    const auto* bytes = legacy ? original.data() : revision.data();
    bool bigEndian = false;
    REQUIRE(amuse::SongState::DetectVersion(bytes, bigEndian) == (legacy ? 0 : 1));
    REQUIRE(bigEndian);
    amuse::SongState song;
    REQUIRE(song.initialize(bytes, true));
    int frame = 0;
    const auto at = [&](double seconds, int program) {
        const int end = static_cast<int>(std::lround(seconds / dt));
        while (frame < end) {
            REQUIRE_FALSE(song.advance(*seq, dt));
            ++frame;
        }
        CAPTURE(seconds, tempoChanges, dt);
        REQUIRE(seq->getChanProgram(0) == program);
    };
    at(.5, 1);
    at(1.5, 2);
    // Native game.bin e0081cdc resets pattern+0x56c/+0x570 to zero when
    // looping, independently of arrangement+0x134 resuming at loopStart.
    // Seeking the pattern to loopStart silences cathy2a's repeated organ.
    const double firstLoop = tempoChanges ? 2.0 : 3.0;
    const double period = tempoChanges ? 1.0 : 2.0;
    for (int loop = 0; loop < 20; ++loop) {
        at(firstLoop + loop * period + .2 * period, 1);
        at(firstLoop + loop * period + .8 * period, 2);
    }
}
