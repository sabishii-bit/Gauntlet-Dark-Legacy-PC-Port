#include "game/netplay/SnapshotBlock.h"

#include <limits>

#include <lz4.h>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLZ");
constexpr u32 kVersion = 2;
constexpr u32 kLz4 = 1U << 16U;
constexpr u32 kBytePlanes = 1U << 17U;
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
static_assert(SnapshotBlock::kMaxBytes < static_cast<usize>(std::numeric_limits<s32>::max()));

std::vector<u8> compress(std::span<const u8> bytes) {
    const auto size = static_cast<s32>(bytes.size());
    std::vector<u8> packed(static_cast<usize>(LZ4_compressBound(size)));
    // LZ4's char pointers describe byte buffers, not reinterpreted objects.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const s32 written = LZ4_compress_default(reinterpret_cast<const char*>(bytes.data()),
                                             reinterpret_cast<char*>(packed.data()), size,
                                             static_cast<s32>(packed.size()));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    packed.resize(written > 0 && written < size ? static_cast<usize>(written) : 0);
    return packed;
}

// Scalars use explicit 32-bit little-endian encoding. Group corresponding bytes
// before compression so float exponents, zero high bytes and nearby IDs form
// runs, instead of each low mantissa byte breaking them. This is a permutation,
// not quantization: all bits (including any trailing partial word) round-trip.
std::vector<u8> planes(std::span<const u8> bytes, bool reverse) {
    const usize words = bytes.size() / 4;
    std::vector<u8> output(bytes.size());
    for (usize lane = 0; lane < 4; ++lane) {
        for (usize i = 0; i < words; ++i) {
            const usize scalar = i * 4 + lane;
            const usize planar = lane * words + i;
            output[reverse ? scalar : planar] = bytes[reverse ? planar : scalar];
        }
    }
    for (usize i = words * 4; i < bytes.size(); ++i) {
        output[i] = bytes[i];
    }
    return output;
}
} // namespace

std::optional<std::vector<u8>> SnapshotBlock::encode(std::span<const u8> bytes,
                                                     Compression compression) {
    if (bytes.empty() || bytes.size() > CombatPacket::kMaxBytes) {
        return std::nullopt;
    }
    std::vector<u8> packed;
    u32 mode = kVersion;
    if (compression == Compression::Automatic) {
        packed = compress(bytes);
        if (!packed.empty()) {
            mode |= kLz4;
        }
        if (bytes.size() >= 1024) {
            auto planar = compress(planes(bytes, false));
            if (!planar.empty() && (packed.empty() || planar.size() < packed.size())) {
                packed = std::move(planar);
                mode = kVersion | kLz4 | kBytePlanes;
            }
        }
    }
    const auto payload = packed.empty() ? bytes : std::span<const u8>(packed);
    std::vector<u8> result;
    result.reserve(kHeaderBytes + payload.size());
    word(result, kMagic);
    word(result, mode);
    word(result, static_cast<u32>(bytes.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

std::optional<std::vector<u8>> SnapshotBlock::decode(std::span<const u8> bytes) {
    if (bytes.size() <= kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic) {
        return std::nullopt;
    }
    const u32 mode = reader.readU32();
    const usize size = reader.readU32();
    if ((mode != kVersion && mode != (kVersion | kLz4) &&
         mode != (kVersion | kLz4 | kBytePlanes)) ||
        size == 0 || size > CombatPacket::kMaxBytes) {
        return std::nullopt;
    }
    const auto payload = bytes.subspan(kHeaderBytes);
    if (mode == kVersion) {
        return payload.size() == size
                   ? std::optional{std::vector<u8>(payload.begin(), payload.end())}
                   : std::nullopt;
    }
    if (payload.size() >= size) {
        return std::nullopt; // the encoder never expands data
    }
    std::vector<u8> unpacked(size);
    // LZ4 requires char pointers to the byte buffers, as with compression above.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const s32 written = LZ4_decompress_safe(
        reinterpret_cast<const char*>(payload.data()), reinterpret_cast<char*>(unpacked.data()),
        static_cast<s32>(payload.size()), static_cast<s32>(size));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (written != static_cast<s32>(size)) {
        return std::nullopt;
    }
    return (mode & kBytePlanes) != 0 ? planes(unpacked, true) : std::move(unpacked);
}
} // namespace gdl::game
