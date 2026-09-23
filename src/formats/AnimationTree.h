#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "formats/KeyframeTrack.h"
#include "formats/ParticleTemplate.h"

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
    s32 particle = -1;                ///< a particle node's template in the file's list, or -1
    Vec3 direction{0.0f, 0.0f, 0.0f}; ///< the way a particle node emits, when not zero
    s32 textureAnimation = -1;        ///< a texture node's animation in the file's list, keyed to
                                      ///< the sequence's frame, or -1

    /** An object node's run of frames in one sequence: the archive object shown at `start`
     * and, each frame after, the next object in the archive, for `frames` frames; nothing
     * outside the run unless it is one frame long, which stays. An empty name shows nothing. */
    struct ObjectFrames {
        std::string object;
        s16 start = 0;
        s16 frames = 0;
    };
    std::vector<ObjectFrames> objectFrames; ///< an object node's, one per sequence
};

/**
 * A texture animation the archive plays on its level: frames cycled into one texture slot
 * (`source` names the first frame's index, or the frame is found by name), or that slot's
 * coordinates scrolled or faded (`source` a mode below).
 */
struct TextureAnimation {
    static constexpr s32 kByName = -1;  ///< the first frame is `frameName`, wherever it lives
    static constexpr s32 kScrollU = -2; ///< coordinates scroll along u
    static constexpr s32 kScrollV = -3; ///< coordinates scroll along v
    static constexpr s32 kFadeIn = -4;
    static constexpr s32 kFadeOut = -5;
    static constexpr s32 kNone = -6;

    std::string name;      ///< the texture the animation plays into
    std::string frameName; ///< the first frame's texture, for kByName
    s32 texture = -1;      ///< the slot in the archive's texture set
    s32 source = -1;       ///< the first frame's index, kByName, or a mode
    s32 frames = 0;        ///< frames in the cycle; negative scrolls the other way
    s32 start = 0;         ///< where in the cycle it begins
    s32 rate = 0;          ///< game frames per step; 0 or 1 steps every frame
    s32 offset = 0;        ///< the frame the cycle counts from
    s16 flag = 0;
    s16 scrollIndex = 0;

    bool scrolls() const { return source == kScrollU || source == kScrollV; }
    bool cycles() const { return source >= 0 || source == kByName; }
};

struct TreeSequence {
    std::string name;
    s16 frameCount = 0;
    s16 frameRate = 0; ///< 900 divided by the frames shown per second
    bool repeats = false;
    bool fixesPosition = false; ///< the root's motion folds into the model when it ends
    u16 flags = 0;
    s32 textureAnimationStart = -1; ///< the first of its own texture animations in the
                                    ///< file's list, keyed to its frame; -1 for none
    s32 textureAnimationCount = 0;
    std::vector<NodeTrack> tracks; ///< one per skeletal node that keys anything
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
    std::vector<TextureAnimation> textureAnimations; ///< the header's list, when it has one
    std::vector<ParticleTemplateRecord> particles;   ///< the header's list, from version 8

    /** Parses the little-endian file, keyframes included; throws FormatError. */
    static AnimationFile parse(std::span<const u8> file);

    std::optional<u32> find(std::string_view name) const;
};

} // namespace gdl::formats
