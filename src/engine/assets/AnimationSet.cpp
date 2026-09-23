#include "engine/assets/AnimationSet.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "engine/assets/ParticleTemplateJson.h"
#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

constexpr std::string_view kManifestName = "animations.json";

TrackInfo parseTrack(const nlohmann::json& track) {
    TrackInfo t;
    t.node = track.at("node").get<std::uint32_t>();
    t.flags = static_cast<std::uint16_t>(track.value("flags", 0U));
    t.frames = track.value("frames", std::vector<std::uint16_t>{});
    t.values = track.value("values", std::vector<float>{});
    if (t.frames.empty() || t.frames.front() != 0) {
        throw std::runtime_error("a track must start with a key at frame 0");
    }
    if (t.values.size() != t.frames.size() * t.channelCount()) {
        throw std::runtime_error("a track's values do not match its keys and channels");
    }
    return t;
}

TreeSequenceInfo parseSequence(const nlohmann::json& sequence) {
    TreeSequenceInfo s;
    s.name = sequence.value("name", std::string{});
    s.frames = sequence.value("frames", 0);
    s.frameRate = sequence.value("frameRate", 0);
    s.repeats = sequence.value("repeats", false);
    s.fixesPosition = sequence.value("fixesPosition", false);
    s.flags = sequence.value("flags", 0U);
    s.textureAnimationStart = sequence.value("textureAnimationStart", -1);
    s.textureAnimationCount = sequence.value("textureAnimationCount", 0);
    for (const nlohmann::json& track : sequence.value("tracks", nlohmann::json::array())) {
        s.tracks.push_back(parseTrack(track));
    }
    return s;
}

TreeNodeInfo parseNode(const nlohmann::json& node, std::size_t before) {
    TreeNodeInfo n;
    n.name = node.value("name", std::string{});
    n.object = normalizeAssetName(node.value("object", std::string{}));
    n.type = node.value("type", 0);
    n.flags = node.value("flags", 0U);
    n.objectFlags = node.value("objectFlags", 0U);
    n.parent = node.value("parent", -1);
    const nlohmann::json& position = node.at("position");
    n.position =
        Vec3{position.at(0).get<float>(), position.at(1).get<float>(), position.at(2).get<float>()};
    n.particle = node.value("particle", -1);
    n.textureAnimation = node.value("textureAnimation", -1);
    if (node.contains("direction")) {
        const nlohmann::json& direction = node.at("direction");
        n.direction = Vec3{direction.at(0).get<float>(), direction.at(1).get<float>(),
                           direction.at(2).get<float>()};
    }
    for (const nlohmann::json& run : node.value("objectFrames", nlohmann::json::array())) {
        TreeNodeInfo::ObjectFrames frames;
        frames.object = normalizeAssetName(run.value("object", std::string{}));
        frames.start = run.value("start", 0);
        frames.frames = run.value("frames", 0);
        n.objectFrames.push_back(std::move(frames));
    }
    if (n.parent >= static_cast<std::int32_t>(before)) {
        throw std::runtime_error("node parent must come before the node");
    }
    return n;
}

/** Points every sequence's nodes at their tracks once the node count is known. */
void indexTracks(TreeInfo& tree) {
    for (TreeSequenceInfo& sequence : tree.sequences) {
        sequence.trackOfNode.assign(tree.nodes.size(), -1);
        for (std::size_t t = 0; t < sequence.tracks.size(); ++t) {
            const std::uint32_t node = sequence.tracks[t].node;
            if (node >= tree.nodes.size()) {
                throw std::runtime_error("a track names a node the tree does not have");
            }
            sequence.trackOfNode[node] = static_cast<std::int32_t>(t);
        }
    }
}

} // namespace

Vec3 TreeInfo::worldPosition(std::size_t node) const {
    Vec3 position{0.0f, 0.0f, 0.0f};
    std::int64_t current = static_cast<std::int64_t>(node);
    std::size_t guard = 0;
    while (current >= 0 && static_cast<std::size_t>(current) < nodes.size() &&
           guard++ < nodes.size()) {
        position += nodes[static_cast<std::size_t>(current)].position;
        current = nodes[static_cast<std::size_t>(current)].parent;
    }
    return position;
}

std::optional<std::uint32_t> TreeInfo::findSequence(std::string_view wanted) const {
    for (std::uint32_t i = 0; i < sequences.size(); ++i) {
        if (sequences[i].name == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> TreeInfo::findNode(std::string_view wanted) const {
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

bool AnimationSet::load(const std::filesystem::path& directory) {
    m_trees.clear();
    m_byName.clear();
    m_textureAnimations.clear();
    m_particles.clear();
    const std::filesystem::path manifest = directory / kManifestName;
    try {
        const std::vector<std::uint8_t> bytes = readFile(manifest);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        for (const nlohmann::json& entry :
             root.value("textureAnimations", nlohmann::json::array())) {
            TextureAnimationInfo animation;
            animation.name = entry.value("name", std::string{});
            animation.frameName = entry.value("frameName", std::string{});
            animation.texture = entry.value("texture", -1);
            animation.source = entry.value("source", -1);
            animation.frames = entry.value("frames", 0);
            animation.start = entry.value("start", 0);
            animation.rate = entry.value("rate", 0);
            animation.offset = entry.value("offset", 0);
            animation.flag = entry.value("flag", TextureAnimationInfo::kFreeRunning);
            m_textureAnimations.push_back(std::move(animation));
        }
        for (const nlohmann::json& entry : root.value("particles", nlohmann::json::array())) {
            m_particles.push_back(readParticleTemplate(entry));
        }
        for (const nlohmann::json& tree : root.at("trees")) {
            TreeInfo info;
            info.name = normalizeAssetName(tree.at("name").get<std::string>());
            info.prefix = tree.value("prefix", std::string{});
            for (const nlohmann::json& sequence :
                 tree.value("sequences", nlohmann::json::array())) {
                info.sequences.push_back(parseSequence(sequence));
            }
            for (const nlohmann::json& node : tree.at("nodes")) {
                info.nodes.push_back(parseNode(node, info.nodes.size()));
            }
            indexTracks(info);
            m_trees.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        log::warn("Animation set {}: {}", manifest.string(), e.what());
        m_trees.clear();
        m_textureAnimations.clear();
        m_particles.clear();
        return false;
    }
    for (std::uint32_t i = 0; i < m_trees.size(); ++i) {
        m_byName.try_emplace(m_trees[i].name, i);
    }
    return !m_trees.empty() || !m_textureAnimations.empty();
}

const TreeInfo& AnimationSet::tree(std::uint32_t index) const {
    GDL_VERIFY(index < m_trees.size(), "animation tree index out of range");
    return m_trees[index];
}

std::optional<std::uint32_t> AnimationSet::find(std::string_view name) const {
    const auto it = m_byName.find(normalizeAssetName(name));
    if (it == m_byName.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace gdl
