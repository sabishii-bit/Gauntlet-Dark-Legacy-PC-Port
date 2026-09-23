#include "engine/codec/Lzss.h"

#include <cstddef>
#include <cstdint>

#include "engine/core/Error.h"

namespace gdl {

namespace {

constexpr std::size_t kMethodWordSize = 4;
constexpr std::uint8_t kMethodStored = 1;
constexpr std::uint32_t kFlagSentinel = 0x10000;
constexpr std::size_t kTokensPerFlagWord = 16;
constexpr std::size_t kTailBytes =
    32; ///< within this many bytes of the end, tokens are checked one at a time
constexpr std::size_t kMinMatch = 3;

} // namespace

std::vector<std::uint8_t> lzssUnpack(std::span<const std::uint8_t> packed) {
    if (packed.size() < kMethodWordSize) {
        throw FormatError("LZSS block is shorter than its method word");
    }

    std::vector<std::uint8_t> out;
    if (packed[0] == kMethodStored) {
        out.assign(packed.begin() + kMethodWordSize, packed.end());
        return out;
    }

    out.reserve(packed.size() * 3);
    std::size_t src = kMethodWordSize;
    const std::size_t end = packed.size();
    std::uint32_t flags = 1;

    while (src != end) {
        if (flags == 1) {
            if (src + 2 > end) {
                throw FormatError("LZSS flag word is truncated");
            }
            flags =
                std::uint32_t{packed[src]} | (std::uint32_t{packed[src + 1]} << 8U) | kFlagSentinel;
            src += 2;
        }
        const std::size_t tokens = (src + kTailBytes > end) ? 1 : kTokensPerFlagWord;
        for (std::size_t i = 0; i < tokens; ++i) {
            if ((flags & 1U) == 0) {
                if (src >= end) {
                    throw FormatError("LZSS literal is truncated");
                }
                out.push_back(packed[src++]);
            } else {
                if (src + 2 > end) {
                    throw FormatError("LZSS match is truncated");
                }
                const std::uint8_t b0 = packed[src];
                const std::uint8_t b1 = packed[src + 1];
                src += 2;
                const std::size_t distance = (std::size_t{b0} & 0xF0U) << 4U | std::size_t{b1};
                const std::size_t length = (std::size_t{b0} & 0x0FU) + kMinMatch;
                if (distance == 0 || distance > out.size()) {
                    throw FormatError("LZSS match points outside the output");
                }
                const std::size_t from = out.size() - distance;
                for (std::size_t k = 0; k < length; ++k) {
                    out.push_back(out[from + k]);
                }
            }
            flags >>= 1U;
        }
    }
    return out;
}

} // namespace gdl
