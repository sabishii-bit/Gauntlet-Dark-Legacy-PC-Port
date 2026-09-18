#include "formats/AnimationTree.h"

#include <bit>
#include <format>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr usize kFileHeaderSize = 8;
constexpr usize kListHeaderSize = 16; ///< the header once it names its texture animations
constexpr usize kTreeInfoSize = 36;
constexpr usize kTreeHeaderSize = 56;
constexpr usize kSequenceSize = 48;
constexpr usize kNodeSize = 60;
constexpr usize kTreeNameSize = 32;
constexpr usize kNodeNameSize = 32;
constexpr usize kPrefixSize = 30;
constexpr usize kTextureAnimationSize = 0x58;
constexpr u16 kNoObjectFlag = 1;
constexpr usize kListHeaderWithParticles = 24; ///< the header once it lists particle templates
constexpr u16 kParticleListVersion = 8;         ///< files older than this hold no such list
constexpr u16 kParticleNodeType = 4;
constexpr u16 kObjectNodeType = 2;
constexpr usize kObjectFramesSize = 40; ///< one sequence's run: name, object, frames, start

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

/** Reads every skeletal node's tracks for every sequence of the tree at `base`. */
void readTracks(std::span<const u8> file, usize base, u32 keyHeaderAt, usize nodesAt,
                TreeDefinition& tree) {
    if (keyHeaderAt == 0) {
        return;
    }
    const KeyHeader header =
        readKeyHeader(file, base + keyHeaderAt, std::format("animation tree {} key header", tree.name));
    const usize sequenceCount = tree.sequences.size();
    for (usize n = 0; n < tree.nodes.size(); ++n) {
        if (tree.nodes[n].type != TreeNodeType::Skeletal) {
            continue;
        }
        const s32 infoOffset = readS32LE(file, nodesAt + n * kNodeSize + 52);
        if (infoOffset < 0) {
            continue;
        }
        const usize infos = base + keyHeaderAt + static_cast<usize>(infoOffset);
        require(file, infos, sequenceCount * KeyHeader::kEntrySize, "track table");
        for (usize s = 0; s < sequenceCount; ++s) {
            TreeSequence& sequence = tree.sequences[s];
            const usize info = infos + s * KeyHeader::kEntrySize;
            if ((readU16LE(file, info) & NodeTrack::kChannels) == 0 || sequence.frameCount <= 0) {
                continue;
            }
            NodeTrack track =
                readKeyTrack(file, info, header, sequence.frameCount,
                             std::format("animation {} {} {}", tree.name, sequence.name,
                                         tree.nodes[n].name));
            track.node = static_cast<u32>(n);
            sequence.tracks.push_back(std::move(track));
        }
    }
}

/** The texture animations the file's header lists, when it has any. */
std::vector<TextureAnimation> readTextureAnimations(std::span<const u8> file) {
    std::vector<TextureAnimation> out;
    if (file.size() < kListHeaderSize) {
        return out;
    }
    const u32 count = readU32LE(file, 8);
    const u32 at = readU32LE(file, 12);
    if (count == 0 || at == 0 || at > file.size() ||
        count > (file.size() - at) / kTextureAnimationSize) {
        return out;
    }
    for (u32 i = 0; i < count; ++i) {
        const usize record = at + usize{i} * kTextureAnimationSize;
        TextureAnimation animation;
        animation.flag = static_cast<s16>(readU16LE(file, record));
        animation.scrollIndex = static_cast<s16>(readU16LE(file, record + 2));
        animation.name = readString(file, record + 4, 32);
        animation.frameName = readString(file, record + 0x24, 32);
        animation.texture = readS32LE(file, record + 0x44);
        animation.source = readS32LE(file, record + 0x48);
        animation.frames = static_cast<s16>(readU16LE(file, record + 0x4C));
        animation.offset = static_cast<s16>(readU16LE(file, record + 0x4E));
        animation.rate = readS32LE(file, record + 0x50);
        animation.start = readS32LE(file, record + 0x54);
        out.push_back(std::move(animation));
    }
    return out;
}

/** The particle templates the file's header lists, from version 8 on. */
std::vector<ParticleTemplateRecord> readParticleTemplates(std::span<const u8> file) {
    std::vector<ParticleTemplateRecord> out;
    if (file.size() < kListHeaderWithParticles || readU16LE(file, 2) < kParticleListVersion) {
        return out;
    }
    const u32 count = readU32LE(file, 16);
    const u32 at = readU32LE(file, 20);
    if (count == 0 || at == 0 || at > file.size() ||
        count > (file.size() - at) / ParticleTemplateRecord::kSize) {
        return out;
    }
    for (u32 i = 0; i < count; ++i) {
        out.push_back(readParticleTemplate(
            file.subspan(at + usize{i} * ParticleTemplateRecord::kSize, ParticleTemplateRecord::kSize)));
    }
    return out;
}

/** Which template a particle node's offset (past the tree header) names, or -1. */
s32 particleIndexOf(std::span<const u8> file, usize base, s32 offset, usize count) {
    if (count == 0 || file.size() < kListHeaderWithParticles) {
        return -1;
    }
    const auto listAt = static_cast<s64>(readU32LE(file, 20));
    const s64 at = static_cast<s64>(base) + static_cast<s64>(kTreeHeaderSize) + offset - listAt;
    if (at < 0 || at % static_cast<s64>(ParticleTemplateRecord::kSize) != 0) {
        return -1;
    }
    const s64 index = at / static_cast<s64>(ParticleTemplateRecord::kSize);
    return index < static_cast<s64>(count) ? static_cast<s32>(index) : -1;
}

} // namespace

/** An object node's frame runs, one per sequence, from the tree's object frame table (the
 * third header field), where the node's data offset points at its first run. */
void readObjectFrames(std::span<const u8> file, usize base, s32 dataOffset,
                      const TreeDefinition& tree, TreeNode& node) {
    if (dataOffset < 0) {
        return;
    }
    const usize runsAt = base + readU32LE(file, base + 8) + static_cast<usize>(dataOffset);
    require(file, runsAt, tree.sequences.size() * kObjectFramesSize, "object frame table");
    for (usize s = 0; s < tree.sequences.size(); ++s) {
        const usize at = runsAt + s * kObjectFramesSize;
        TreeNode::ObjectFrames run;
        std::string object = readString(file, at, kTreeNameSize);
        if (object.size() > AnimationFile::kObjectNameLength) {
            object.resize(AnimationFile::kObjectNameLength);
        }
        run.object = normalizeAssetName(object);
        run.frames = static_cast<s16>(readU16LE(file, at + 36));
        run.start = static_cast<s16>(readU16LE(file, at + 38));
        node.objectFrames.push_back(std::move(run));
    }
}

AnimationFile AnimationFile::parse(std::span<const u8> file) {
    if (file.size() < kFileHeaderSize) {
        throw FormatError("animation file is too small for its header");
    }
    const u16 treeCount = readU16LE(file, 0);
    const u32 infosAt = readU32LE(file, 4);
    require(file, infosAt, usize{treeCount} * kTreeInfoSize, "tree table");

    AnimationFile out;
    out.textureAnimations = readTextureAnimations(file);
    out.particles = readParticleTemplates(file);
    for (u16 i = 0; i < treeCount; ++i) {
        const usize info = infosAt + usize{i} * kTreeInfoSize;
        TreeDefinition tree;
        tree.name = readString(file, info, kTreeNameSize);
        const u32 base = readU32LE(file, info + 32);
        require(file, base, kTreeHeaderSize, "tree header");
        const u32 sequencesAt = readU32LE(file, base);
        const u32 keyHeaderAt = readU32LE(file, base + 4);
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
            sequence.fixesPosition = (readU16LE(file, at + 38) & 1U) != 0;
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
            if ((readU16LE(file, at + 44) & 0xFFU) == kParticleNodeType) {
                // A particle node's name holds the way it emits after twenty characters.
                node.particle = particleIndexOf(file, base, readS32LE(file, at + 52),
                                                out.particles.size());
                node.direction =
                    Vec3{readF32(file, at + 20), readF32(file, at + 24), readF32(file, at + 28)};
            }
            if ((readU16LE(file, at + 44) & 0xFFU) == kObjectNodeType) {
                readObjectFrames(file, base, readS32LE(file, at + 52), tree, node);
            }
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
        readTracks(file, base, keyHeaderAt, usize{base} + nodesAt, tree);
        out.trees.push_back(std::move(tree));
    }
    return out;
}

std::optional<u32> AnimationFile::find(std::string_view name) const {
    const std::string wanted = normalizeAssetName(name);
    for (u32 i = 0; i < trees.size(); ++i) {
        if (normalizeAssetName(trees[i].name) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::formats
