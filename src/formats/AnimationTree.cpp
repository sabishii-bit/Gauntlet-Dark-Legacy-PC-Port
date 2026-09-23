#include "formats/AnimationTree.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr std::size_t kFileHeaderSize = 8;
constexpr std::size_t kListHeaderSize = 16; ///< the header once it names its texture animations
constexpr std::size_t kTreeInfoSize = 36;
constexpr std::size_t kTreeHeaderSize = 56;
constexpr std::size_t kSequenceSize = 48;
constexpr std::size_t kNodeSize = 60;
constexpr std::size_t kTreeNameSize = 32;
constexpr std::size_t kNodeNameSize = 32;
constexpr std::size_t kPrefixSize = 30;
constexpr std::size_t kTextureAnimationSize = 0x58;
constexpr std::uint16_t kNoObjectFlag = 1;
constexpr std::size_t kListHeaderWithParticles =
    24;                                           ///< the header once it lists particle templates
constexpr std::uint16_t kParticleListVersion = 8; ///< files older than this hold no such list
constexpr std::uint16_t kParticleNodeType = 4;
constexpr std::uint16_t kTextureNodeType = 3;
constexpr std::uint16_t kObjectNodeType = 2;
constexpr std::size_t kObjectFramesSize = 40; ///< one sequence's run: name, object, frames, start

std::string readString(std::span<const std::uint8_t> file, std::size_t at, std::size_t width) {
    if (at > file.size() || width > file.size() - at) {
        throw FormatError("animation file string lies outside the file");
    }
    std::string text;
    for (std::size_t i = 0; i < width && file[at + i] != 0; ++i) {
        text.push_back(static_cast<char>(file[at + i]));
    }
    return text;
}

void require(std::span<const std::uint8_t> file, std::size_t at, std::size_t size,
             std::string_view what) {
    if (at > file.size() || size > file.size() - at) {
        throw FormatError(std::format("animation file {} lies outside the file", what));
    }
}

float readF32(std::span<const std::uint8_t> file, std::size_t at) {
    return std::bit_cast<float>(readU32LE(file, at));
}

/** Reads every skeletal node's tracks for every sequence of the tree at `base`. */
void readTracks(std::span<const std::uint8_t> file, std::size_t base, std::uint32_t keyHeaderAt,
                std::size_t nodesAt, TreeDefinition& tree) {
    if (keyHeaderAt == 0) {
        return;
    }
    const KeyHeader header = readKeyHeader(file, base + keyHeaderAt,
                                           std::format("animation tree {} key header", tree.name));
    const std::size_t sequenceCount = tree.sequences.size();
    for (std::size_t n = 0; n < tree.nodes.size(); ++n) {
        if (tree.nodes[n].type != TreeNodeType::Skeletal) {
            continue;
        }
        const std::int32_t infoOffset = readS32LE(file, nodesAt + n * kNodeSize + 52);
        if (infoOffset < 0) {
            continue;
        }
        const std::size_t infos = base + keyHeaderAt + static_cast<std::size_t>(infoOffset);
        require(file, infos, sequenceCount * KeyHeader::kEntrySize, "track table");
        for (std::size_t s = 0; s < sequenceCount; ++s) {
            TreeSequence& sequence = tree.sequences[s];
            const std::size_t info = infos + s * KeyHeader::kEntrySize;
            if ((readU16LE(file, info) & NodeTrack::kChannels) == 0 || sequence.frameCount <= 0) {
                continue;
            }
            NodeTrack track = readKeyTrack(
                file, info, header, sequence.frameCount,
                std::format("animation {} {} {}", tree.name, sequence.name, tree.nodes[n].name));
            track.node = static_cast<std::uint32_t>(n);
            sequence.tracks.push_back(std::move(track));
        }
    }
}

/** The texture animations the file's header lists, when it has any. */
std::vector<TextureAnimation> readTextureAnimations(std::span<const std::uint8_t> file) {
    std::vector<TextureAnimation> out;
    if (file.size() < kListHeaderSize) {
        return out;
    }
    const std::uint32_t count = readU32LE(file, 8);
    const std::uint32_t at = readU32LE(file, 12);
    if (count == 0 || at == 0 || at > file.size() ||
        count > (file.size() - at) / kTextureAnimationSize) {
        return out;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t record = at + std::size_t{i} * kTextureAnimationSize;
        TextureAnimation animation;
        animation.flag = static_cast<std::int16_t>(readU16LE(file, record));
        animation.scrollIndex = static_cast<std::int16_t>(readU16LE(file, record + 2));
        animation.name = readString(file, record + 4, 32);
        animation.frameName = readString(file, record + 0x24, 32);
        animation.texture = readS32LE(file, record + 0x44);
        animation.source = readS32LE(file, record + 0x48);
        animation.frames = static_cast<std::int16_t>(readU16LE(file, record + 0x4C));
        animation.offset = static_cast<std::int16_t>(readU16LE(file, record + 0x4E));
        animation.rate = readS32LE(file, record + 0x50);
        animation.start = readS32LE(file, record + 0x54);
        out.push_back(std::move(animation));
    }
    return out;
}

/** The particle templates the file's header lists, from version 8 on. */
std::vector<ParticleTemplateRecord> readParticleTemplates(std::span<const std::uint8_t> file) {
    std::vector<ParticleTemplateRecord> out;
    if (file.size() < kListHeaderWithParticles || readU16LE(file, 2) < kParticleListVersion) {
        return out;
    }
    const std::uint32_t count = readU32LE(file, 16);
    const std::uint32_t at = readU32LE(file, 20);
    if (count == 0 || at == 0 || at > file.size() ||
        count > (file.size() - at) / ParticleTemplateRecord::kSize) {
        return out;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(readParticleTemplate(file.subspan(
            at + std::size_t{i} * ParticleTemplateRecord::kSize, ParticleTemplateRecord::kSize)));
    }
    return out;
}

/** Which template a particle node's offset (past the tree header) names, or -1. */
std::int32_t particleIndexOf(std::span<const std::uint8_t> file, std::size_t base,
                             std::int32_t offset, std::size_t count) {
    if (count == 0 || file.size() < kListHeaderWithParticles) {
        return -1;
    }
    const auto listAt = static_cast<std::int64_t>(readU32LE(file, 20));
    const std::int64_t at = static_cast<std::int64_t>(base) +
                            static_cast<std::int64_t>(kTreeHeaderSize) + offset - listAt;
    if (at < 0 || at % static_cast<std::int64_t>(ParticleTemplateRecord::kSize) != 0) {
        return -1;
    }
    const std::int64_t index = at / static_cast<std::int64_t>(ParticleTemplateRecord::kSize);
    return index < static_cast<std::int64_t>(count) ? static_cast<std::int32_t>(index) : -1;
}

/** Which of the file's texture animations lies `offset` bytes past `from`, or -1: the way a
 * texture node's data offset (from the tree's sequence table) and a sequence's own list name
 * them. */
std::int32_t textureAnimationIndexOf(std::span<const std::uint8_t> file, std::int64_t from,
                                     std::int64_t offset) {
    if (file.size() < kListHeaderSize) {
        return -1;
    }
    const std::int64_t count = readU32LE(file, 8);
    const std::int64_t listAt = readU32LE(file, 12);
    const std::int64_t at = from + offset - listAt;
    if (count == 0 || listAt == 0 || at < 0 ||
        at % static_cast<std::int64_t>(kTextureAnimationSize) != 0) {
        return -1;
    }
    const std::int64_t index = at / static_cast<std::int64_t>(kTextureAnimationSize);
    return index < count ? static_cast<std::int32_t>(index) : -1;
}

} // namespace

/** An object node's frame runs, one per sequence, from the tree's object frame table (the
 * third header field), where the node's data offset points at its first run. */
void readObjectFrames(std::span<const std::uint8_t> file, std::size_t base, std::int32_t dataOffset,
                      const TreeDefinition& tree, TreeNode& node) {
    if (dataOffset < 0) {
        return;
    }
    const std::size_t runsAt =
        base + readU32LE(file, base + 8) + static_cast<std::size_t>(dataOffset);
    require(file, runsAt, tree.sequences.size() * kObjectFramesSize, "object frame table");
    for (std::size_t s = 0; s < tree.sequences.size(); ++s) {
        const std::size_t at = runsAt + s * kObjectFramesSize;
        TreeNode::ObjectFrames run;
        std::string object = readString(file, at, kTreeNameSize);
        if (object.size() > AnimationFile::kObjectNameLength) {
            object.resize(AnimationFile::kObjectNameLength);
        }
        run.object = normalizeAssetName(object);
        run.frames = static_cast<std::int16_t>(readU16LE(file, at + 36));
        run.start = static_cast<std::int16_t>(readU16LE(file, at + 38));
        node.objectFrames.push_back(std::move(run));
    }
}

AnimationFile AnimationFile::parse(std::span<const std::uint8_t> file) {
    if (file.size() < kFileHeaderSize) {
        throw FormatError("animation file is too small for its header");
    }
    const std::uint16_t treeCount = readU16LE(file, 0);
    const std::uint32_t infosAt = readU32LE(file, 4);
    require(file, infosAt, std::size_t{treeCount} * kTreeInfoSize, "tree table");

    AnimationFile out;
    out.textureAnimations = readTextureAnimations(file);
    out.particles = readParticleTemplates(file);
    for (std::uint16_t i = 0; i < treeCount; ++i) {
        const std::size_t info = infosAt + std::size_t{i} * kTreeInfoSize;
        TreeDefinition tree;
        tree.name = readString(file, info, kTreeNameSize);
        const std::uint32_t base = readU32LE(file, info + 32);
        require(file, base, kTreeHeaderSize, "tree header");
        const std::uint32_t sequencesAt = readU32LE(file, base);
        const std::uint32_t keyHeaderAt = readU32LE(file, base + 4);
        const std::uint32_t nodesAt = readU32LE(file, base + 12);
        const auto nodeCount = static_cast<std::int32_t>(readU32LE(file, base + 16));
        const auto sequenceCount = static_cast<std::int32_t>(readU32LE(file, base + 20));
        tree.prefix = readString(file, base + 24, kPrefixSize);
        if (nodeCount < 0 || sequenceCount < 0) {
            throw FormatError(std::format("animation tree {} has negative counts", tree.name));
        }

        require(file, std::size_t{base} + sequencesAt,
                static_cast<std::size_t>(sequenceCount) * kSequenceSize, "sequence table");
        for (std::int32_t s = 0; s < sequenceCount; ++s) {
            const std::size_t at =
                std::size_t{base} + sequencesAt + static_cast<std::size_t>(s) * kSequenceSize;
            TreeSequence sequence;
            sequence.name = readString(file, at, kTreeNameSize);
            sequence.frameCount = static_cast<std::int16_t>(readU16LE(file, at + 32));
            sequence.frameRate = static_cast<std::int16_t>(readU16LE(file, at + 34));
            sequence.repeats = readU16LE(file, at + 36) != 0;
            sequence.fixesPosition = (readU16LE(file, at + 38) & 1U) != 0;
            sequence.flags = readU16LE(file, at + 42);
            // Its own texture animations: a count and the index of the first in the list.
            const auto texmods = static_cast<std::int16_t>(readU16LE(file, at + 40));
            const std::int32_t first = readS32LE(file, at + 44);
            const auto listed = static_cast<std::int32_t>(out.textureAnimations.size());
            if (texmods > 0 && first >= 0 && first + texmods <= listed) {
                sequence.textureAnimationStart = first;
                sequence.textureAnimationCount = texmods;
            }
            tree.sequences.push_back(std::move(sequence));
        }

        require(file, std::size_t{base} + nodesAt, static_cast<std::size_t>(nodeCount) * kNodeSize,
                "node table");
        for (std::int32_t n = 0; n < nodeCount; ++n) {
            const std::size_t at =
                std::size_t{base} + nodesAt + static_cast<std::size_t>(n) * kNodeSize;
            TreeNode node;
            node.name = readString(file, at, kNodeNameSize);
            node.position =
                Vec3{readF32(file, at + 32), readF32(file, at + 36), readF32(file, at + 40)};
            node.type = static_cast<TreeNodeType>(readU16LE(file, at + 44) & 0xFFU);
            node.flags = readU16LE(file, at + 46);
            node.objectFlags = readU32LE(file, at + 48);
            node.parent = static_cast<std::int32_t>(readU32LE(file, at + 56));
            if ((readU16LE(file, at + 44) & 0xFFU) == kParticleNodeType) {
                // A particle node's name holds the way it emits after twenty characters.
                node.particle =
                    particleIndexOf(file, base, readS32LE(file, at + 52), out.particles.size());
                node.direction =
                    Vec3{readF32(file, at + 20), readF32(file, at + 24), readF32(file, at + 28)};
            }
            if ((readU16LE(file, at + 44) & 0xFFU) == kObjectNodeType) {
                readObjectFrames(file, base, readS32LE(file, at + 52), tree, node);
            }
            if ((readU16LE(file, at + 44) & 0xFFU) == kTextureNodeType) {
                // A texture node's data lies in the file's animation list, reached from the
                // tree's sequence table.
                node.textureAnimation = textureAnimationIndexOf(
                    file, static_cast<std::int64_t>(base) + sequencesAt, readS32LE(file, at + 52));
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
        readTracks(file, base, keyHeaderAt, std::size_t{base} + nodesAt, tree);
        out.trees.push_back(std::move(tree));
    }
    return out;
}

std::optional<std::uint32_t> AnimationFile::find(std::string_view name) const {
    const std::string wanted = normalizeAssetName(name);
    for (std::uint32_t i = 0; i < trees.size(); ++i) {
        if (normalizeAssetName(trees[i].name) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::formats
