#include "engine/assets/AnimationSet.h"

#include <exception>

#include <nlohmann/json.hpp>

#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

constexpr std::string_view kManifestName = "animations.json";

} // namespace

Vec3 TreeInfo::worldPosition(usize node) const {
    Vec3 position{0.0f, 0.0f, 0.0f};
    s64 current = static_cast<s64>(node);
    usize guard = 0;
    while (current >= 0 && static_cast<usize>(current) < nodes.size() && guard++ < nodes.size()) {
        position += nodes[static_cast<usize>(current)].position;
        current = nodes[static_cast<usize>(current)].parent;
    }
    return position;
}

bool AnimationSet::load(const std::filesystem::path& directory) {
    m_trees.clear();
    m_byName.clear();
    const std::filesystem::path manifest = directory / kManifestName;
    try {
        const std::vector<u8> bytes = readFile(manifest);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        for (const nlohmann::json& tree : root.at("trees")) {
            TreeInfo info;
            info.name = normalizeAssetName(tree.at("name").get<std::string>());
            info.prefix = tree.value("prefix", std::string{});
            for (const nlohmann::json& sequence :
                 tree.value("sequences", nlohmann::json::array())) {
                TreeSequenceInfo s;
                s.name = sequence.value("name", std::string{});
                s.frames = sequence.value("frames", 0);
                s.frameRate = sequence.value("frameRate", 0);
                s.repeats = sequence.value("repeats", false);
                info.sequences.push_back(std::move(s));
            }
            for (const nlohmann::json& node : tree.at("nodes")) {
                TreeNodeInfo n;
                n.name = node.value("name", std::string{});
                n.object = normalizeAssetName(node.value("object", std::string{}));
                n.type = node.value("type", 0);
                n.flags = node.value("flags", 0U);
                n.objectFlags = node.value("objectFlags", 0U);
                n.parent = node.value("parent", -1);
                const nlohmann::json& position = node.at("position");
                n.position = Vec3{position.at(0).get<f32>(), position.at(1).get<f32>(),
                                  position.at(2).get<f32>()};
                if (n.parent >= static_cast<s32>(info.nodes.size())) {
                    throw std::runtime_error("node parent must come before the node");
                }
                info.nodes.push_back(std::move(n));
            }
            m_trees.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        log::warn("Animation set {}: {}", manifest.string(), e.what());
        m_trees.clear();
        return false;
    }
    for (u32 i = 0; i < m_trees.size(); ++i) {
        m_byName.try_emplace(m_trees[i].name, i);
    }
    return !m_trees.empty();
}

const TreeInfo& AnimationSet::tree(u32 index) const {
    GDL_VERIFY(index < m_trees.size(), "animation tree index out of range");
    return m_trees[index];
}

std::optional<u32> AnimationSet::find(std::string_view name) const {
    const auto it = m_byName.find(normalizeAssetName(name));
    if (it == m_byName.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace gdl
