#pragma once

#include <bit>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/assets/ParticleTemplate.h"
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
    Vec3 position{0.0f, 0.0f, 0.0f};  ///< relative to the parent
    s32 particle = -1;                ///< the set's particle template a particle node emits, or -1
    Vec3 direction{0.0f, 0.0f, 0.0f}; ///< the way it emits, when not zero
    s32 textureAnimation = -1;        ///< the set's texture animation a texture node plays, keyed
                                      ///< to the sequence's frame, or -1

    /** One sequence's run of object frames: the set's object shown at `start` and, each
     * frame after, the next object in the set, for `frames` frames; nothing outside the run
     * unless it is one frame long, which stays. An empty name shows nothing. */
    struct ObjectFrames {
        std::string object;
        s32 start = 0;
        s32 frames = 0;
    };
    std::vector<ObjectFrames> objectFrames; ///< an object node's, one per sequence

    static constexpr s32 kParticleType = 4;
    static constexpr s32 kObjectType = 2; ///< a node whose object changes with the frame
    bool chrome() const { return (objectFlags & kChromeFlag) != 0; }
    /** The object's blending and independent depth-write/compare policies. */
    bool additive() const { return (objectFlags & kAdditiveFlag) != 0; }
    bool writesDepth() const { return (objectFlags & kNoDepthWriteFlag) == 0; }
    bool testsDepth() const { return (objectFlags & kNoDepthTestFlag) == 0; }
    static constexpr u32 kChromeFlag = 0x8000;
    static constexpr u32 kAdditiveFlag = 0x800000;
    static constexpr u32 kNoDepthWriteFlag = 0x80;
    static constexpr u32 kNoDepthTestFlag = 0x40;
};

/**
 * One node's keys in one sequence: which of the nine channels it keys (rotation, position and
 * scale, x y z each), the frames holding keys and that many values per key. Channels a track
 * does not key stay at rest: rotation and position 0, scale 1.
 */
struct TrackInfo {
    static constexpr u16 kChannels = 0x777;
    static constexpr u16 kPitchYawRoll =
        0x8000; ///< rotate pitch, yaw then roll; else roll, yaw, pitch
    static constexpr u32 kChannelCount = 9;

    u32 node = 0;
    u16 flags = 0;
    std::vector<u16> frames; ///< ascending, starting at 0
    std::vector<f32> values; ///< channelCount() per key

    static constexpr u16 channelBit(u32 channel) {
        return static_cast<u16>(1U << (channel + channel / 3));
    }
    bool has(u32 channel) const { return (flags & channelBit(channel)) != 0; }
    bool pitchYawRoll() const { return (flags & kPitchYawRoll) != 0; }
    u32 channelCount() const {
        return static_cast<u32>(std::popcount(static_cast<u32>(flags & kChannels)));
    }
};

struct TreeSequenceInfo {
    std::string name;
    s32 frames = 0;
    s32 frameRate = 0; ///< 900 divided by the frames shown per second; 0 plays at 30
    bool repeats = false;
    bool fixesPosition = false;
    u32 flags = 0;
    s32 textureAnimationStart = -1; ///< the first of the set's texture animations that
                                    ///< this sequence keys to its frame; -1 for none
    s32 textureAnimationCount = 0;
    std::vector<TrackInfo> tracks;
    std::vector<s32> trackOfNode; ///< per tree node: its track's index, -1 for none

    /** Object/texture nodes count backward for bit 0. Transform tracks still use
     * the playback frame: their keys already describe the sequence's movement. */
    s32 effectFrame(s32 frame) const {
        return (flags & 1U) != 0 && frames > 0 ? frames - frame - 1 : frame;
    }

    /** The keys node `node` plays in this sequence, or null when it stays at rest. */
    const TrackInfo* track(usize node) const {
        return node < trackOfNode.size() && trackOfNode[node] >= 0
                   ? &tracks[static_cast<usize>(trackOfNode[node])]
                   : nullptr;
    }
};

/**
 * A texture animation an archive plays on its level: frames cycled into one texture slot
 * (`source` the first frame's index in the same set, or the frame found by `frameName`
 * wherever it lives), or that slot's coordinates scrolled along u or v.
 */
struct TextureAnimationInfo {
    static constexpr s32 kByName = -1;
    static constexpr s32 kScrollU = -2;
    static constexpr s32 kScrollV = -3;
    static constexpr s32 kFadeOut = -4;
    static constexpr s32 kFadeIn = -5;
    static constexpr s32 kFreeRunning = -1; ///< the flag of one stepped on the game clock

    std::string name;
    std::string frameName;
    s32 texture = -1; ///< the slot the animation plays into
    s32 source = -1;
    s32 frames = 0; ///< frames in the cycle; negative scrolls the other way
    s32 start = 0;  ///< where in the cycle it begins
    s32 rate = 0;   ///< game frames per step; 0 or 1 steps every frame
    s32 offset = 0; ///< the frame the cycle counts from: a keyed one's first sequence frame
    s32 flag = kFreeRunning; ///< anything else is keyed to a sequence's frame by a tree

    bool scrolls() const { return source == kScrollU || source == kScrollV; }
    bool cycles() const { return source >= 0 || source == kByName; }
    bool fades() const { return source == kFadeOut || source == kFadeIn; }
    /** Stepped on the game clock, rather than keyed to a sequence's frame. */
    bool freeRunning() const { return flag == kFreeRunning; }
};

/** A node hierarchy from an unpacked animation file, with the objects each node draws. */
struct TreeInfo {
    std::string name;
    std::string prefix;
    std::vector<TreeNodeInfo> nodes;
    std::vector<TreeSequenceInfo> sequences;

    /** Position of a node relative to the tree root (its own offset plus every ancestor's). */
    Vec3 worldPosition(usize node) const;
    std::optional<u32> findSequence(std::string_view wanted) const;
    std::optional<u32> findNode(std::string_view wanted) const;
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
    const std::vector<TextureAnimationInfo>& textureAnimations() const {
        return m_textureAnimations;
    }
    const std::vector<ParticleTemplate>& particleTemplates() const { return m_particles; }

private:
    std::vector<TreeInfo> m_trees;
    std::vector<TextureAnimationInfo> m_textureAnimations;
    std::vector<ParticleTemplate> m_particles;
    std::unordered_map<std::string, u32> m_byName;
};

} // namespace gdl
