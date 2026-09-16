#include "engine/codec/Lzss.h"

#include "engine/core/Error.h"

namespace gdl {

namespace {

constexpr usize kMethodWordSize = 4;
constexpr u8 kMethodStored = 1;
constexpr u32 kFlagSentinel = 0x10000;
constexpr usize kTokensPerFlagWord = 16;
constexpr usize kTailBytes =
    32; ///< within this many bytes of the end, tokens are checked one at a time
constexpr usize kMinMatch = 3;

} // namespace

std::vector<u8> lzssUnpack(std::span<const u8> packed) {
    if (packed.size() < kMethodWordSize) {
        throw FormatError("LZSS block is shorter than its method word");
    }

    std::vector<u8> out;
    if (packed[0] == kMethodStored) {
        out.assign(packed.begin() + kMethodWordSize, packed.end());
        return out;
    }

    out.reserve(packed.size() * 3);
    usize src = kMethodWordSize;
    const usize end = packed.size();
    u32 flags = 1;

    while (src != end) {
        if (flags == 1) {
            if (src + 2 > end) {
                throw FormatError("LZSS flag word is truncated");
            }
            flags = u32{packed[src]} | (u32{packed[src + 1]} << 8U) | kFlagSentinel;
            src += 2;
        }
        const usize tokens = (src + kTailBytes > end) ? 1 : kTokensPerFlagWord;
        for (usize i = 0; i < tokens; ++i) {
            if ((flags & 1U) == 0) {
                if (src >= end) {
                    throw FormatError("LZSS literal is truncated");
                }
                out.push_back(packed[src++]);
            } else {
                if (src + 2 > end) {
                    throw FormatError("LZSS match is truncated");
                }
                const u8 b0 = packed[src];
                const u8 b1 = packed[src + 1];
                src += 2;
                const usize distance = (usize{b0} & 0xF0U) << 4U | usize{b1};
                const usize length = (usize{b0} & 0x0FU) + kMinMatch;
                if (distance == 0 || distance > out.size()) {
                    throw FormatError("LZSS match points outside the output");
                }
                const usize from = out.size() - distance;
                for (usize k = 0; k < length; ++k) {
                    out.push_back(out[from + k]);
                }
            }
            flags >>= 1U;
        }
    }
    return out;
}

} // namespace gdl
