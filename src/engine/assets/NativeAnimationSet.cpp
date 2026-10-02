#include <exception>
#include <utility>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/NativeParticleTemplate.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"

#include "formats/AnimationTree.h"

namespace gdl {

namespace {

TreeInfo nativeTree(formats::TreeDefinition source) {
    TreeInfo tree;
    tree.name = normalizeAssetName(source.name);
    tree.prefix = std::move(source.prefix);
    for (auto& node : source.nodes) {
        TreeNodeInfo info;
        info.name = std::move(node.name);
        info.object = normalizeAssetName(node.object);
        info.type = static_cast<s32>(node.type);
        info.flags = node.flags;
        info.objectFlags = node.objectFlags;
        info.parent = node.parent;
        info.position = node.position;
        info.particle = node.particle;
        info.direction = node.direction;
        info.textureAnimation = node.textureAnimation;
        if (info.parent >= static_cast<s32>(tree.nodes.size())) {
            throw FormatError("node parent must come before the node");
        }
        for (const auto& run : node.objectFrames) {
            info.objectFrames.push_back({normalizeAssetName(run.object), run.start, run.frames});
        }
        tree.nodes.push_back(std::move(info));
    }
    for (auto& sequence : source.sequences) {
        TreeSequenceInfo info;
        info.name = std::move(sequence.name);
        info.frames = sequence.frameCount;
        info.frameRate = sequence.frameRate;
        info.repeats = sequence.repeats;
        info.fixesPosition = sequence.fixesPosition;
        info.flags = sequence.flags;
        info.textureAnimationStart = sequence.textureAnimationStart;
        info.textureAnimationCount = sequence.textureAnimationCount;
        info.trackOfNode.assign(tree.nodes.size(), -1);
        for (auto& track : sequence.tracks) {
            TrackInfo converted;
            converted.node = track.node;
            converted.flags = track.flags;
            converted.frames = std::move(track.frames);
            converted.values = std::move(track.values);
            if (converted.node >= tree.nodes.size() || converted.frames.empty() ||
                converted.frames.front() != 0 ||
                converted.values.size() != converted.frames.size() * converted.channelCount()) {
                throw FormatError("invalid native animation track");
            }
            info.trackOfNode[converted.node] = static_cast<s32>(info.tracks.size());
            info.tracks.push_back(std::move(converted));
        }
        tree.sequences.push_back(std::move(info));
    }
    return tree;
}

} // namespace

bool AnimationSet::loadNative(const std::filesystem::path& file) {
    try {
        auto source = formats::AnimationFile::parse(readFile(file));
        for (auto& animation : source.textureAnimations) {
            TextureAnimationInfo info;
            info.name = std::move(animation.name);
            info.frameName = std::move(animation.frameName);
            info.texture = animation.texture;
            info.source = animation.source;
            info.frames = animation.frames;
            info.start = animation.start;
            info.rate = animation.rate;
            info.offset = animation.offset;
            info.flag = animation.flag;
            m_textureAnimations.push_back(std::move(info));
        }
        for (const auto& particle : source.particles) {
            m_particles.push_back(nativeParticle(particle));
        }
        for (auto& tree : source.trees) {
            m_trees.push_back(nativeTree(std::move(tree)));
        }
        for (u32 i = 0; i < m_trees.size(); ++i) {
            m_byName.try_emplace(m_trees[i].name, i);
        }
        return !m_trees.empty() || !m_textureAnimations.empty();
    } catch (const std::exception& e) {
        log::warn("Native animation set {}: {}", file.string(), e.what());
        m_trees.clear();
        m_textureAnimations.clear();
        m_particles.clear();
        m_byName.clear();
        return false;
    }
}

} // namespace gdl
