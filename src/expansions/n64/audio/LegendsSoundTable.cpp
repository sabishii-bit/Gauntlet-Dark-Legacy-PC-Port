#include "expansions/n64/audio/LegendsSoundTable.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <utility>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"
#include "engine/io/Sha256.h"

namespace gdl::game {

std::vector<LegendsSoundEntry> decodeLegendsSoundTable(std::span<const u8> program) {
    if (program.size() < 0x7b514 - 0x400 || program.size() > usize{4} * 1024 * 1024) {
        throw FormatError("unsupported native N64 sound routing program");
    }
    Sha256 hash;
    hash.update(program);
    if (hash.hex() != "0b60632eb50c7e77957a5c28106b4f353ba69dccbe0b37f1f68d37a36fd6e585") {
        throw FormatError("N64 sound routing requires the audited US program");
    }
    // e007a0c0 resolves tokens against 124 twenty-byte rows at e007ab4c.
    // +0 token, +2 replacement, +4 gain, +5 pan, +16 frequency multiplier.
    const usize count = readU32BE(program, 0x7b510 - 0x400);
    if (count != 124) {
        throw FormatError("unsupported native N64 sound routing count");
    }
    std::vector<LegendsSoundEntry> entries;
    entries.reserve(count);
    for (usize i = 0; i < count; ++i) {
        const usize at = 0x7ab4c - 0x400 + i * 20;
        LegendsSoundEntry entry{readU16BE(program, at),
                                readU16BE(program, at + 2),
                                program[at + 4],
                                program[at + 5],
                                std::bit_cast<f32>(readU32BE(program, at + 16)),
                                {}};
        const u32 nameAddress = readU32BE(program, at + 8);
        if (nameAddress != 0) {
            constexpr u32 kProgramBase = 0xe0000400;
            if (nameAddress < kProgramBase || nameAddress - kProgramBase >= program.size()) {
                throw FormatError("invalid native N64 sound name address");
            }
            const usize begin = nameAddress - kProgramBase;
            const usize end = std::min(begin + 64, program.size());
            const auto first = program.begin() + static_cast<std::ptrdiff_t>(begin);
            const auto last = program.begin() + static_cast<std::ptrdiff_t>(end);
            const auto zero = std::find(first, last, 0);
            if (zero == last) {
                throw FormatError("unterminated native N64 sound name");
            }
            entry.name.assign(first, zero);
            entry.name = toLowerAscii(entry.name);
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::optional<u32> legendsSoundToken(std::span<const LegendsSoundEntry> entries,
                                     std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    const auto folded = toLowerAscii(name);
    for (usize i = 0; i < entries.size(); ++i) {
        if (!entries[i].name.empty() && toLowerAscii(entries[i].name) == folded) {
            return 0x10000U | static_cast<u32>(i);
        }
    }
    return std::nullopt;
}

} // namespace gdl::game
