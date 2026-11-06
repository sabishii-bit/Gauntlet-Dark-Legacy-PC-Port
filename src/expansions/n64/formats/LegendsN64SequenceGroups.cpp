#include "expansions/n64/formats/LegendsN64SequenceGroups.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "engine/core/Error.h"

namespace gdl::formats {
void combineLegendsN64SequenceGroups(AnimationFile& animation,
                                     std::span<const std::string_view> selected) {
    std::vector<TreeDefinition> combined;
    std::map<std::string, usize, std::less<>> groups;
    for (auto& tree : animation.trees) {
        const auto colon = tree.name.find(':');
        if (colon == std::string::npos || tree.sequences.size() != 1) {
            throw FormatError("N64 sequence group: invalid decoded tree");
        }
        std::string group = tree.name.substr(0, colon);
        if (!selected.empty() && std::ranges::find(selected, group) == selected.end()) {
            combined.push_back(std::move(tree));
            continue;
        }
        // Load-time commands have no drawable hierarchy and may intentionally
        // repeat the same FOURCC. Keep their original independent identities.
        if (tree.nodes.empty()) {
            combined.push_back(std::move(tree));
            continue;
        }
        auto [found, inserted] = groups.emplace(group, combined.size());
        if (inserted) {
            tree.name = std::move(group);
            for (auto& node : tree.nodes) {
                if (node.objectFrames.empty()) {
                    node.objectFrames.push_back(
                        {node.object, 0, static_cast<s16>(node.object.empty() ? 0 : 1), false});
                }
            }
            combined.push_back(std::move(tree));
            continue;
        }
        auto& destination = combined[found->second];
        if (destination.nodes.size() != tree.nodes.size()) {
            throw FormatError("N64 sequence group: BODY size changes in " + group);
        }
        for (usize i = 0; i < tree.nodes.size(); ++i) {
            auto& node = destination.nodes[i];
            const auto& incoming = tree.nodes[i];
            if (node.name != incoming.name || node.parent != incoming.parent ||
                node.position != incoming.position || node.objectFlags != incoming.objectFlags ||
                node.flags != incoming.flags || node.particle != incoming.particle ||
                node.direction != incoming.direction ||
                node.textureAnimation != incoming.textureAnimation) {
                throw FormatError("N64 sequence group: BODY binding changes in " + group);
            }
            // Static models must agree. Sequence-local property5/6 variants are
            // carried by objectFrames and retain their own discrete/morph mode.
            if (node.object != incoming.object && node.type != TreeNodeType::Object &&
                incoming.type != TreeNodeType::Object) {
                throw FormatError("N64 sequence group: static model changes in " + group);
            }
            if (incoming.type == TreeNodeType::Object) {
                node.type = TreeNodeType::Object;
            }
            node.objectFrames.push_back(
                incoming.objectFrames.empty()
                    ? TreeNode::ObjectFrames{incoming.object, 0,
                                             static_cast<s16>(incoming.object.empty() ? 0 : 1),
                                             false}
                    : incoming.objectFrames.front());
        }
        const auto& name = tree.sequences.front().name;
        for (const auto& sequence : destination.sequences) {
            if (sequence.name == name) {
                throw FormatError("N64 sequence group: duplicate sequence");
            }
        }
        destination.sequences.push_back(std::move(tree.sequences.front()));
    }
    animation.trees = std::move(combined);
}
} // namespace gdl::formats
