#include "formats/AnimationTree.h"

#include <bit>
#include <format>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr usize kFileHeaderSize = 8;
constexpr usize kTreeInfoSize = 36;
constexpr usize kTreeHeaderSize = 56;
constexpr usize kSequenceSize = 48;
constexpr usize kNodeSize = 60;
constexpr usize kTreeNameSize = 32;
constexpr usize kNodeNameSize = 32;
constexpr usize kPrefixSize = 30;
constexpr u16 kNoObjectFlag = 1;

std::string readString(std::span<const u8> file, usize at, usize width) {
    if (at > file.size() || width > file.size() - at) {
        throw FormatError("animation file string lies outside the file");
    }
    std::string text;
    for (usize i = 0; i < width && file[at + i] != 0; ++i) {
        text.push_back(static_cast<char>(file[at + i]));
    }
    return text;
}

void require(std::span<const u8> file, usize at, usize size, std::string_view what) {
    if (at > file.size() || size > file.size() - at) {
        throw FormatError(std::format("animation file {} lies outside the file", what));
    }
}

f32 readF32(std::span<const u8> file, usize at) {
    return std::bit_cast<f32>(readU32LE(file, at));
}

} // namespace

AnimationFile AnimationFile::parse(std::span<const u8> file) {
    if (file.size() < kFileHeaderSize) {
        throw FormatError("animation file is too small for its header");
    }
    const u16 treeCount = readU16LE(file, 0);
    const u32 infosAt = readU32LE(file, 4);
    require(file, infosAt, usize{treeCount} * kTreeInfoSize, "tree table");

    AnimationFile out;
    for (u16 i = 0; i < treeCount; ++i) {
        const usize info = infosAt + usize{i} * kTreeInfoSize;
        TreeDefinition tree;
        tree.name = readString(file, info, kTreeNameSize);
        const u32 base = readU32LE(file, info + 32);
        require(file, base, kTreeHeaderSize, "tree header");
        const u32 sequencesAt = readU32LE(file, base);
        const u32 nodesAt = readU32LE(file, base + 12);
        const auto nodeCount = static_cast<s32>(readU32LE(file, base + 16));
        const auto sequenceCount = static_cast<s32>(readU32LE(file, base + 20));
        tree.prefix = readString(file, base + 24, kPrefixSize);
        if (nodeCount < 0 || sequenceCount < 0) {
            throw FormatError(std::format("animation tree {} has negative counts", tree.name));
        }

        require(file, usize{base} + sequencesAt, static_cast<usize>(sequenceCount) * kSequenceSize,
                "sequence table");
        for (s32 s = 0; s < sequenceCount; ++s) {
            const usize at = usize{base} + sequencesAt + static_cast<usize>(s) * kSequenceSize;
            TreeSequence sequence;
            sequence.name = readString(file, at, kTreeNameSize);
            sequence.frameCount = static_cast<s16>(readU16LE(file, at + 32));
            sequence.frameRate = static_cast<s16>(readU16LE(file, at + 34));
            sequence.repeats = readU16LE(file, at + 36) != 0;
            sequence.flags = readU16LE(file, at + 42);
            tree.sequences.push_back(std::move(sequence));
        }

        require(file, usize{base} + nodesAt, static_cast<usize>(nodeCount) * kNodeSize,
                "node table");
        for (s32 n = 0; n < nodeCount; ++n) {
            const usize at = usize{base} + nodesAt + static_cast<usize>(n) * kNodeSize;
            TreeNode node;
            node.name = readString(file, at, kNodeNameSize);
            node.position =
                Vec3{readF32(file, at + 32), readF32(file, at + 36), readF32(file, at + 40)};
            node.type = static_cast<TreeNodeType>(readU16LE(file, at + 44) & 0xFFU);
            node.flags = readU16LE(file, at + 46);
            node.objectFlags = readU32LE(file, at + 48);
            node.parent = static_cast<s32>(readU32LE(file, at + 56));
            if (node.parent >= n) {
                throw FormatError(
                    std::format("animation tree {} node {} has a parent after it", tree.name, n));
            }
            if ((node.flags & kNoObjectFlag) == 0 && !node.name.empty()) {
                std::string object = tree.prefix + node.name;
                if (object.size() > kObjectNameLength) {
                    object.resize(kObjectNameLength);
                }
                node.object = normalizeAssetName(object);
            }
            tree.nodes.push_back(std::move(node));
        }
        out.trees.push_back(std::move(tree));
    }
    return out;
}

std::optional<u32> AnimationFile::find(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    for (u32 i = 0; i < trees.size(); ++i) {
        if (normalizeAssetName(trees[i].name) == key) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::formats
