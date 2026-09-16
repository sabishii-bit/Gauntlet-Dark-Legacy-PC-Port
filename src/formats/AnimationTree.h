#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

/** Kinds of node an animation tree can hold. */
enum class TreeNodeType : u8 {
    Group = 0,    ///< plain hierarchy node
    Skeletal = 1, ///< may carry keyframed transforms
    Object = 2,   ///< swaps between object frames
    Texture = 3,  ///< plays a texture animation on the nodes below
    Particles = 4,
};

struct TreeNode {
    std::string name;
    std::string object; ///< archive object drawn at this node, empty for none
    TreeNodeType type = TreeNodeType::Group;
    u16 flags = 0;
    u32 objectFlags = 0; ///< scene flags of the object: 0x8000 chrome, 0x40 no z test, ...
    Vec3 position{0.0f, 0.0f, 0.0f}; ///< relative to the parent
    s32 parent = -1;
};

struct TreeSequence {
    std::string name;
    s16 frameCount = 0;
    s16 frameRate = 0;
    bool repeats = false;
    u16 flags = 0;
};

struct TreeDefinition {
    std::string name;
    std::string prefix; ///< object names are the prefix plus the node name, cut to 15 characters
    std::vector<TreeNode> nodes;
    std::vector<TreeSequence> sequences;
};

/** An archive's animation file: every animation tree it defines. */
struct AnimationFile {
    static constexpr usize kObjectNameLength = 15;
    static constexpr u32 kChromeFlag = 0x8000;

    std::vector<TreeDefinition> trees;

    /** Parses the little-endian file; throws FormatError. Keyframes are not read yet. */
    static AnimationFile parse(std::span<const u8> file);

    std::optional<u32> find(std::string_view name) const;
};

} // namespace gdl::formats
