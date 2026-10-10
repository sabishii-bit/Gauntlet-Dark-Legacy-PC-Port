#include "game/netplay/FighterSnapshot.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
bool bounded(f32 value, f32 low, f32 high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool affine(const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 3; ++row) {
            if (!bounded(value[column][row], -1'000'000, 1'000'000)) {
                return false;
            }
        }
    }
    return value[0].w == 0 && value[1].w == 0 && value[2].w == 0 && value[3].w == 1;
}
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void wide(std::vector<u8>& bytes, u64 value) {
    word(bytes, static_cast<u32>(value));
    word(bytes, static_cast<u32>(value >> 32U));
}
u64 wide(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32U);
}
void real(std::vector<u8>& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
void matrix(std::vector<u8>& bytes, const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 3; ++row) {
            real(bytes, value[column][row]);
        }
    }
}
Mat4 matrix(ByteReader& reader) {
    Mat4 value{1};
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 3; ++row) {
            value[column][row] = real(reader);
        }
    }
    return value;
}
} // namespace
bool FighterMeshState::valid() const {
    if (actor > 16 || incarnation == 0 || part == 0 || part > 65535 || resource == 0 ||
        resource > 65535 || !affine(placement) || sequence > 65535 ||
        !bounded(frame, 0, 1'000'000) || !bounded(textureClock, 0, 1'000'000'000.0f) ||
        !bounded(alpha, 0, 1) || skin > 65535 || (flags & ~7U) != 0 ||
        nodes.size() > FighterPacket::kMaxTreeNodes) {
        return false;
    }
    return std::ranges::all_of(nodes, [](const auto& node) {
        return affine(node.transform) && node.sequence <= 65535 &&
               bounded(node.frame, 0, 1'000'000) && bounded(node.alpha, 0, 1);
    });
}
bool FighterPacket::valid(std::span<const FighterMeshState> meshes) {
    if (meshes.size() > kMaxMeshes) {
        return false;
    }
    usize nodes = 0;
    for (usize i = 0; i < meshes.size(); ++i) {
        const auto& mesh = meshes[i];
        if (!mesh.valid() || (i > 0 && (meshes[i - 1].key() >= mesh.key() ||
                                        (meshes[i - 1].actor == mesh.actor &&
                                         meshes[i - 1].incarnation != mesh.incarnation)))) {
            return false;
        }
        nodes += mesh.nodes.size();
    }
    return nodes <= kMaxNodes;
}
std::optional<std::vector<u8>> FighterPacket::encode(std::span<const FighterMeshState> meshes) {
    if (!valid(meshes)) {
        return std::nullopt;
    }
    usize nodes = 0;
    for (const auto& mesh : meshes) {
        nodes += mesh.nodes.size();
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + meshes.size() * kMeshBytes + nodes * kNodeBytes);
    word(bytes, static_cast<u32>(meshes.size()));
    word(bytes, static_cast<u32>(nodes));
    for (const auto& mesh : meshes) {
        word(bytes, mesh.actor);
        wide(bytes, mesh.incarnation);
        word(bytes, mesh.part);
        word(bytes, mesh.resource);
        matrix(bytes, mesh.placement);
        word(bytes, mesh.sequence);
        real(bytes, mesh.frame);
        real(bytes, mesh.textureClock);
        real(bytes, mesh.alpha);
        word(bytes, u32{mesh.tint.r} | (u32{mesh.tint.g} << 8U) | (u32{mesh.tint.b} << 16U) |
                        (u32{mesh.tint.a} << 24U));
        word(bytes, mesh.skin);
        word(bytes, mesh.flags);
        word(bytes, static_cast<u32>(mesh.nodes.size()));
        for (const auto& node : mesh.nodes) {
            matrix(bytes, node.transform);
            wide(bytes, node.generation);
            word(bytes, node.sequence);
            real(bytes, node.frame);
            real(bytes, node.alpha);
            word(bytes, node.flash ? 1 : 0);
        }
    }
    return bytes;
}
std::optional<std::vector<FighterMeshState>> FighterPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader data(bytes);
    const usize count = data.readU32();
    const usize nodes = data.readU32();
    if (count > kMaxMeshes || nodes > kMaxNodes ||
        bytes.size() != kHeaderBytes + count * kMeshBytes + nodes * kNodeBytes) {
        return std::nullopt;
    }
    std::vector<FighterMeshState> result;
    result.reserve(count);
    usize consumed = 0;
    for (usize i = 0; i < count; ++i) {
        if (data.remaining() < kMeshBytes) {
            return std::nullopt;
        }
        FighterMeshState mesh;
        mesh.actor = data.readU32();
        mesh.incarnation = wide(data);
        mesh.part = data.readU32();
        mesh.resource = data.readU32();
        mesh.placement = matrix(data);
        mesh.sequence = data.readU32();
        mesh.frame = real(data);
        mesh.textureClock = real(data);
        mesh.alpha = real(data);
        const auto color = data.readU32();
        mesh.tint = Color::rgba(static_cast<u8>(color), static_cast<u8>(color >> 8U),
                                static_cast<u8>(color >> 16U), static_cast<u8>(color >> 24U));
        mesh.skin = data.readU32();
        mesh.flags = data.readU32();
        const usize length = data.readU32();
        if (length > kMaxTreeNodes || length > nodes - consumed ||
            data.remaining() < length * kNodeBytes) {
            return std::nullopt;
        }
        consumed += length;
        mesh.nodes.reserve(length);
        for (usize n = 0; n < length; ++n) {
            FighterMeshState::Node node;
            node.transform = matrix(data);
            node.generation = wide(data);
            node.sequence = data.readU32();
            node.frame = real(data);
            node.alpha = real(data);
            const auto flash = data.readU32();
            if (flash > 1) {
                return std::nullopt;
            }
            node.flash = flash != 0;
            mesh.nodes.push_back(node);
        }
        result.push_back(std::move(mesh));
    }
    return consumed == nodes && data.remaining() == 0 && valid(result)
               ? std::optional{std::move(result)}
               : std::nullopt;
}
} // namespace gdl::game
