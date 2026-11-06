#include "expansions/n64/audio/MortBank.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/ByteReader.h"
#include "engine/platform/Paths.h"

#include "expansions/shared/audio/RestoredSpeech.h"

namespace gdl {
namespace {

constexpr usize kSamplesPerBlock = 160;
constexpr usize kMaximumDirectoryBytes = usize{64} * 1024 * 1024;

struct Clip {
    usize offset = 0;
    usize bytes = 0;
    u16 blocks = 0;
    u16 sampleRate = 0;
};

// Native e0096720 reads low bits first from big-endian 32-bit words. This is
// neither the byte order nor the framing used by ordinary GSM .gsm files.
class Bits {
public:
    explicit Bits(std::span<const u8> bytes) : m_bytes(bytes) {}

    u32 read(u32 count) {
        if (m_bit > m_bytes.size() * 8 || count > m_bytes.size() * 8 - m_bit) {
            throw FormatError("truncated MORT frame");
        }
        u32 result = 0;
        for (u32 i = 0; i < count; ++i, ++m_bit) {
            result |= ((readU32BE(m_bytes, m_bit / 32 * 4) >> (m_bit % 32)) & 1U) << i;
        }
        return result;
    }

private:
    std::span<const u8> m_bytes;
    usize m_bit = 96;
};

struct Subframe {
    u32 lag = 0;
    u32 gain = 0;
    u32 grid = 0;
    u32 amplitude = 0;
    std::array<u32, 13> pulses{};
};

struct Block {
    bool silent = false;
    std::array<u32, 8> reflection{};
    std::array<Subframe, 4> subframes{};
};

class Blocks {
public:
    explicit Blocks(std::span<const u8> bytes) : m_bits(bytes) {}

    Block next(usize remaining) {
        if (m_active == 0 && m_silent == 0) {
            if (m_bits.read(1) != 0) {
                m_silent = m_bits.read(4) + 1;
            } else {
                m_active = m_bits.read(7) + 1;
            }
            if (m_active + m_silent > remaining) {
                throw FormatError("MORT run exceeds declared frame count");
            }
        }
        Block block;
        if (m_silent != 0) {
            --m_silent;
            block.silent = true;
            return block;
        }
        constexpr std::array<u32, 8> kWidths{6, 6, 5, 5, 4, 4, 3, 3};
        for (usize i = 0; i < kWidths.size(); ++i) {
            block.reflection[i] = m_bits.read(kWidths[i]);
        }
        for (auto& sub : block.subframes) {
            sub.lag = m_bits.read(7);
            sub.gain = m_bits.read(2);
            sub.grid = m_bits.read(2);
            sub.amplitude = m_bits.read(6);
            for (auto& pulse : sub.pulses) {
                pulse = m_bits.read(3);
            }
        }
        --m_active;
        return block;
    }

private:
    Bits m_bits;
    u32 m_active = 0;
    u32 m_silent = 0;
};

// The original MIPS synthesis intentionally wraps intermediate words. Preserve
// that arithmetic without signed-overflow UB; saturate only where native does.
s32 word(s64 value) {
    return std::bit_cast<s32>(static_cast<u32>(value));
}

s16 half(s32 value) {
    return std::bit_cast<s16>(static_cast<u16>(value));
}

s32 multiply(s32 a, s32 b) {
    return word(static_cast<s64>(a) * b + 0x4000) >> 15;
}

class Synthesis {
public:
    std::array<s16, kSamplesPerBlock> decode(const Block& block) {
        std::array<s16, kSamplesPerBlock> output{};
        // Native silence writes zero PCM without advancing any predictor state.
        if (block.silent) {
            return output;
        }
        // e0097620: residual pulse excitation and inverse APCM quantization.
        constexpr std::array<s32, 8> kSmallAmplitude{-57, -41, -29, -25, -15, -13, -11, -9};
        constexpr std::array<s32, 4> kGain{3277, 11469, 21299, 32767};
        for (const auto& sub : block.subframes) {
            s32 exponent = (static_cast<s32>(sub.amplitude) - 8) >> 3;
            u32 mantissa = sub.amplitude & 7U;
            if (sub.amplitude < kSmallAmplitude.size()) {
                const s32 packed = kSmallAmplitude[sub.amplitude];
                exponent = packed >> 4;
                mantissa = static_cast<u32>(packed) & 7U;
            }
            std::array<s16, 40> residual{};
            for (usize i = 0; i < sub.pulses.size(); ++i) {
                const s32 value = static_cast<s32>(sub.pulses[i]) * 8192 - 28672;
                const s32 scale = static_cast<s32>(mantissa) * 2048 + 18431;
                const u32 round = 1U << (static_cast<u32>(5 - exponent) & 31U);
                residual[sub.grid + i * 3] =
                    half(word(static_cast<s64>(multiply(value, scale)) + round) >>
                         (static_cast<u32>(6 - exponent) & 31U));
            }
            // e0097714 retains the last valid pitch lag (40..120), shifts its
            // history before each 40-sample subframe, and stores wrapped 16-bit output.
            if (sub.lag >= 40 && sub.lag <= 120) {
                m_lag = sub.lag;
            }
            for (usize i = 0; i < 120; ++i) {
                m_longTerm[i] = m_longTerm[i + 40];
            }
            for (usize i = 0; i < residual.size(); ++i) {
                m_longTerm[120 + i] =
                    half(multiply(m_longTerm[120 + i - m_lag], kGain[sub.gain]) + residual[i]);
            }
        }
        // e00961b0: native log-area-ratio dequantization, including its rounding.
        struct Coefficient {
            s32 centre;
            s32 scale;
            s32 bias;
        };
        constexpr std::array<Coefficient, 8> kCoefficients{{
            {32, 0xcccc00, 0x4000},
            {32, 0xcccc00, 0x4000},
            {16, 0xcccc00, -0x332f000},
            {16, 0xcccc00, 0x4003c00},
            {8, 0x12c5c00, -0x36e4e4},
            {8, 0x1111000, 0x3bbf800},
            {4, 0x1eb7800, 0x147936c},
            {4, 0x1d03000, 0x40d6b40},
        }};
        std::array<s16, 8> current{};
        for (usize i = 0; i < current.size(); ++i) {
            const auto& coefficient = kCoefficients[i];
            const s64 value =
                (static_cast<s64>(block.reflection[i]) - coefficient.centre) * coefficient.scale +
                coefficient.bias;
            current[i] = half((word(value) >> 15) * 2);
        }
        // e0097994/e0097ae8: four interpolation windows of 13, 14, 13, 120 samples.
        constexpr std::array<usize, 5> kWindows{0, 13, 27, 40, 160};
        for (usize section = 0; section < 4; ++section) {
            std::array<s32, 8> reflection{};
            for (usize i = 0; i < reflection.size(); ++i) {
                const s32 now = current[i];
                const s32 previous = m_previous[i];
                s32 value = now;
                if (section == 0) {
                    value = (now >> 2) + (previous >> 1) + (previous >> 2);
                } else if (section == 1) {
                    value = (now >> 1) + (previous >> 1);
                } else if (section == 2) {
                    value = (now >> 1) + (now >> 2) + (previous >> 2);
                }
                const s32 magnitude = std::abs(value);
                const s32 converted = magnitude < 11059   ? magnitude * 2
                                      : magnitude < 20070 ? magnitude + 11059
                                                          : (magnitude >> 2) + 26112;
                reflection[i] = std::min(converted, 32767) * (value < 0 ? -1 : 1);
            }
            // e00977d0: 32-bit lattice history (unlike libgsm's saturated 16-bit
            // intermediates), de-emphasis, final saturation and 13-bit truncation.
            for (usize i = kWindows[section]; i < kWindows[section + 1]; ++i) {
                s32 value =
                    word(static_cast<s64>(m_longTerm[i]) - multiply(m_filter[7], reflection[7]));
                for (s32 j = 6; j >= 0; --j) {
                    value = word(static_cast<s64>(value) - multiply(m_filter[j], reflection[j]));
                    m_filter[j + 1] =
                        word(static_cast<s64>(m_filter[j]) + multiply(value, reflection[j]));
                }
                m_filter[0] = value;
                m_deemphasis = word(static_cast<s64>(value) + multiply(m_deemphasis, 0x6e14));
                const s32 doubled = word(static_cast<s64>(m_deemphasis) * 2);
                output[i] = half(std::clamp(doubled, -32768, 32767) & 0xfff8);
            }
        }
        m_deemphasis = half(m_deemphasis);
        m_previous = current;
        return output;
    }

private:
    std::array<s16, 160> m_longTerm{};
    std::array<s32, 8> m_filter{};
    std::array<s16, 8> m_previous{};
    u32 m_lag = 40;
    s32 m_deemphasis = 0;
};

class MortSource final : public StreamSource {
public:
    MortSource(std::shared_ptr<const std::vector<u8>> bytes, std::vector<Clip> clips)
        : m_bytes(std::move(bytes)), m_clips(std::move(clips)) {
        rewind();
    }

    AudioStreamDesc desc() const override { return {m_clips.front().sampleRate, 1}; }

    bool read(std::vector<f32>& out, usize frames) override {
        if (frames == 0 || m_clip == m_clips.size()) {
            return false;
        }
        const usize before = out.size();
        const usize wanted = std::min<usize>(frames, 32768);
        for (usize i = 0; i < wanted; ++i) {
            if (m_sample == kSamplesPerBlock) {
                if (m_block == m_clips[m_clip].blocks) {
                    if (++m_clip == m_clips.size()) {
                        break;
                    }
                    startClip();
                }
                m_output = m_synthesis.decode(m_blocks->next(m_clips[m_clip].blocks - m_block));
                ++m_block;
                m_sample = 0;
            }
            out.push_back(static_cast<f32>(m_output[m_sample++]) / 32768.0f);
        }
        return out.size() != before;
    }

    void rewind() override {
        m_clip = 0;
        startClip();
    }

private:
    void startClip() {
        const auto& clip = m_clips[m_clip];
        m_blocks = std::make_unique<Blocks>(
            std::span<const u8>(*m_bytes).subspan(clip.offset, clip.bytes));
        m_synthesis = {};
        m_block = 0;
        m_sample = kSamplesPerBlock;
    }

    std::shared_ptr<const std::vector<u8>> m_bytes;
    std::vector<Clip> m_clips;
    std::unique_ptr<Blocks> m_blocks;
    Synthesis m_synthesis;
    std::array<s16, kSamplesPerBlock> m_output{};
    usize m_clip = 0;
    usize m_block = 0;
    usize m_sample = kSamplesPerBlock;
};

} // namespace

struct MortBank::Data {
    std::shared_ptr<const std::vector<u8>> bytes;
    std::vector<Clip> clips;
};

bool MortBank::open(const std::filesystem::path& path) {
    m_data.reset();
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto length = file.tellg();
        if (!file || length < 4 || static_cast<u64>(length) > kMaximumDirectoryBytes) {
            throw FormatError("missing or invalid MORT directory size");
        }
        std::vector<u8> bytes(static_cast<usize>(length));
        file.seekg(0);
        file.read(std::bit_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            throw FormatError("truncated MORT directory");
        }
        return open(bytes);
    } catch (const std::exception& error) {
        log::warn("MORT directory {}: {}", paths::utf8(path), error.what());
        return false;
    }
}

bool MortBank::open(std::span<const u8> bytes) {
    m_data.reset();
    try {
        if (bytes.size() < 4 || bytes.size() > kMaximumDirectoryBytes) {
            throw FormatError("invalid MORT directory size");
        }
        const usize count = readU32BE(bytes, 0);
        if (count == 0 || count > 1024 || count > (bytes.size() - 4) / 4) {
            throw FormatError("invalid MORT directory count");
        }
        auto data = std::make_shared<Data>();
        data->clips.reserve(count);
        usize totalBlocks = 0;
        for (usize i = 0; i < count; ++i) {
            const usize begin = readU32BE(bytes, 4 + i * 4);
            const usize end = i + 1 == count ? bytes.size() : readU32BE(bytes, 8 + i * 4);
            if (begin < 4 + count * 4 || begin % 4 != 0 || end % 4 != 0 || end > bytes.size() ||
                begin > end || end - begin < 16) {
                throw FormatError("invalid MORT clip extent");
            }
            const auto clipBytes = bytes.subspan(begin, end - begin);
            const u16 blocks = readU16BE(clipBytes, 4);
            const u16 rate = readU16BE(clipBytes, 6);
            const u32 words = readU32BE(clipBytes, 8);
            if (std::memcmp(clipBytes.data(), "MORT", 4) != 0 || blocks == 0 || rate < 1000 ||
                rate > 48000 || words != clipBytes.size() / 4) {
                throw FormatError("unsupported or malformed MORT clip header");
            }
            totalBlocks += blocks;
            if (totalBlocks > usize{1024} * 1024) {
                throw FormatError("MORT directory exceeds decoding budget");
            }
            // Preflight every frame before a mixer thread can receive a source.
            // Thus truncated runs/bitstreams fail at open, never mid-playback.
            Blocks parser(clipBytes);
            for (usize block = 0; block < blocks; ++block) {
                parser.next(blocks - block);
            }
            data->clips.push_back({begin, end - begin, blocks, rate});
        }
        data->bytes = std::make_shared<const std::vector<u8>>(bytes.begin(), bytes.end());
        m_data = std::move(data);
        return true;
    } catch (const std::exception& error) {
        log::warn("MORT directory: {}", error.what());
        return false;
    }
}

usize MortBank::size() const {
    return m_data ? m_data->clips.size() : 0;
}

bool MortBank::contains(u16 id) const {
    return m_data && id < m_data->clips.size();
}

std::shared_ptr<StreamSource> MortBank::create(u16 id) const {
    return create(std::span<const u16>(&id, 1));
}

std::shared_ptr<StreamSource> MortBank::create(std::span<const u16> ids) const {
    if (!m_data || ids.empty() || ids.size() > 6) {
        return {};
    }
    std::vector<Clip> clips;
    clips.reserve(ids.size());
    for (const u16 id : ids) {
        if (!contains(id) ||
            (!clips.empty() && clips.front().sampleRate != m_data->clips[id].sampleRate)) {
            return {};
        }
        clips.push_back(m_data->clips[id]);
    }
    return std::make_shared<MortSource>(m_data->bytes, std::move(clips));
}

std::shared_ptr<StreamSource> MortBank::createRestored(u16 id) const {
    return createRestored(std::span<const u16>(&id, 1));
}

std::shared_ptr<StreamSource> MortBank::createRestored(std::span<const u16> ids) const {
    if (!m_data || ids.empty() || ids.size() > 6) {
        return {};
    }
    std::vector<std::shared_ptr<StreamSource>> clips;
    clips.reserve(ids.size());
    for (const u16 id : ids) {
        auto clip = create(id);
        if (!clip ||
            (!clips.empty() && clip->desc().sampleRate != clips.front()->desc().sampleRate)) {
            return {};
        }
        clips.push_back(std::move(clip));
    }
    return std::make_shared<RestoredSpeech>(std::move(clips));
}

} // namespace gdl
