#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

struct TreeNodeInfo {
    std::string name;
    std::string object; ///< archive object drawn at this node, empty for none
    s32 type = 0;
    u32 flags = 0;
    u32 objectFlags = 0; ///< 0x8000 marks a chrome (environment mapped) object
    s32 parent = -1;
    Vec3 position{0.0f, 0.0f, 0.0f}; ///< relative to the parent

    bool chrome() const { return (objectFlags & kChromeFlag) != 0; }
    static constexpr u32 kChromeFlag = 0x8000;
};

struct TreeSequenceInfo {
    std::string name;
    s32 frames = 0;
    s32 frameRate = 0;
    bool repeats = false;
};

/** A node hierarchy from an unpacked animation file, with the objects each node draws. */
struct TreeInfo {
    std::string name;
    std::string prefix;
    std::vector<TreeNodeInfo> nodes;
    std::vector<TreeSequenceInfo> sequences;

    /** Position of a node relative to the tree root (its own offset plus every ancestor's). */
    Vec3 worldPosition(usize node) const;
};

/** The animation trees of one unpacked archive. */
class AnimationSet {
public:
    /** Reads `directory/animations.json`; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return !m_trees.empty(); }
    usize size() const { return m_trees.size(); }
    const TreeInfo& tree(u32 index) const;
    std::optional<u32> find(std::string_view name) const;

private:
    std::vector<TreeInfo> m_trees;
    std::unordered_map<std::string, u32> m_byName;
};

} // namespace gdl
