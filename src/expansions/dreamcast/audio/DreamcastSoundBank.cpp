#include "expansions/dreamcast/audio/DreamcastSoundBank.h"

#include <algorithm>
#include <array>
#include <bit>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl {
namespace {
void require(bool value) {
    if (!value)
        throw FormatError("Invalid Dreamcast sound bank");
}
} // namespace

std::vector<DreamcastBankSample> decodeDreamcastSoundBank(std::span<const u8> bytes) {
    require(bytes.size() >= 4 && bytes.size() <= usize{64} * 1024 * 1024);
    const usize count = readU32LE(bytes, 0);
    require(count > 0 && count <= 4096 && count <= (bytes.size() - 4) / 44);
    std::vector<DreamcastBankSample> result;
    result.reserve(count);
    usize previousEnd = 4 + count * 44;
    usize totalFrames = 0;
    for (usize i = 0; i < count; ++i) {
        const usize at = 4 + i * 44;
        const usize offset = readU32LE(bytes, at + 4);
        const usize length = readU32LE(bytes, at + 8);
        const u32 rate = readU32LE(bytes, at + 12);
        const u32 loop = readU32LE(bytes, at + 16);
        const u32 bits = readU32LE(bytes, at + 20);
        require(readU32LE(bytes, at) == 1 && rate >= 8000 && rate <= 192000 && loop <= 1 &&
                (bits == 4 || bits == 8) && offset >= previousEnd && offset <= bytes.size() &&
                length > 0 && length <= bytes.size() - offset);
        for (usize field = 24; field < 44; field += 4)
            require(readU32LE(bytes, at + field) == 0);
        previousEnd = offset + length;
        const usize frames = length * (bits == 4 ? 2 : 1);
        totalFrames += frames;
        require(totalFrames <= usize{32} * 1024 * 1024);
        DreamcastBankSample sample;
        sample.clip.sampleRate = rate;
        sample.clip.channels = 1;
        sample.loop = loop != 0;
        sample.clip.samples.reserve(frames);
        s32 history = 0, step = 127;
        constexpr std::array<s32, 8> kFactors{230, 230, 230, 230, 307, 409, 512, 614};
        for (const u8 byte : bytes.subspan(offset, length)) {
            if (bits == 8) {
                // amUtilGetAicaSampleType8c102990 maps8 to signed PCM8;
                // 4 selects AICA long-stream ADPCM, not a PCM quantizer.
                sample.clip.samples.push_back(static_cast<f32>(std::bit_cast<s8>(byte)) / 128);
                continue;
            }
            // AICA's low nibble precedes its high nibble, as in the native STR
            // path. Reset the predictor for each independent sample, not each byte.
            for (const u32 shift : {0U, 4U}) {
                const u8 nibble = (byte >> shift) & 15U;
                const s32 delta = step * (static_cast<s32>(nibble & 7U) * 2 + 1) / 8;
                history = std::clamp(history + ((nibble & 8U) ? -delta : delta), -32768, 32767);
                step = std::clamp(step * kFactors[nibble & 7U] / 256, 127, 24576);
                sample.clip.samples.push_back(static_cast<f32>(history) / 32768);
            }
        }
        result.push_back(std::move(sample));
    }
    return result;
}

} // namespace gdl
