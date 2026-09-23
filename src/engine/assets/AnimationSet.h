#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/assets/ParticleTemplate.h"
#include "engine/math/Math.h"

namespace gdl {

struct TreeNodeInfo {
    std::string name;
    std::string object; ///< archive object drawn at this node, empty for none
    std::int32_t type = 0;
    std::uint32_t flags = 0;
    std::uint32_t objectFlags = 0; ///< 0x8000 marks a chrome (environment mapped) object
    std::int32_t parent = -1;
    Vec3 position{0.0f, 0.0f, 0.0f};  ///< relative to the parent
    std::int32_t particle = -1;       ///< the set's particle template a particle node emits, or -1
    Vec3 direction{0.0f, 0.0f, 0.0f}; ///< the way it emits, when not zero
    std::int32_t textureAnimation = -1; ///< the set's texture animation a texture node plays, keyed
                                        ///< to the sequence's frame, or -1

    /** One sequence's run of object frames: the set's object shown at `start` and, each
     * frame after, the next object in the set, for `frames` frames; nothing outside the run
     * unless it is one frame long, which stays. An empty name shows nothing. */
    struct ObjectFrames {
        std::string object;
        std::int32_t start = 0;
        std::int32_t frames = 0;
    };
    std::vector<ObjectFrames> objectFrames; ///< an object node's, one per sequence

    static constexpr std::int32_t kParticleType = 4;
    static constexpr std::int32_t kObjectType = 2; ///< a node whose object changes with the frame
    bool chrome() const { return (objectFlags & kChromeFlag) != 0; }
    /** Whether the object adds onto the frame, and whether it leaves depth unwritten. */
    bool additive() const { return (objectFlags & kAdditiveFlag) != 0; }
    bool writesDepth() const { return (objectFlags & kNoDepthWriteFlag) == 0; }
    static constexpr std::uint32_t kChromeFlag = 0x8000;
    static constexpr std::uint32_t kAdditiveFlag = 0x800000;
    static constexpr std::uint32_t kNoDepthWriteFlag = 0x80;
};

/**
 * One node's keys in one sequence: which of the nine channels it keys (rotation, position and
 * scale, x y z each), the frames holding keys and that many values per key. Channels a track
 * does not key stay at rest: rotation and position 0, scale 1.
 */
struct TrackInfo {
    static constexpr std::uint16_t kChannels = 0x777;
    static constexpr std::uint16_t kPitchYawRoll =
        0x8000; ///< rotate pitch, yaw then roll; else roll, yaw, pitch
    static constexpr std::uint32_t kChannelCount = 9;

    std::uint32_t node = 0;
    std::uint16_t flags = 0;
    std::vector<std::uint16_t> frames; ///< ascending, starting at 0
    std::vector<float> values;         ///< channelCount() per key

    static constexpr std::uint16_t channelBit(std::uint32_t channel) {
        return static_cast<std::uint16_t>(1U << (channel + channel / 3));
    }
    bool has(std::uint32_t channel) const { return (flags & channelBit(channel)) != 0; }
    bool pitchYawRoll() const { return (flags & kPitchYawRoll) != 0; }
    std::uint32_t channelCount() const {
        return static_cast<std::uint32_t>(
            std::popcount(static_cast<std::uint32_t>(flags & kChannels)));
    }
};

struct TreeSequenceInfo {
    std::string name;
    std::int32_t frames = 0;
    std::int32_t frameRate = 0; ///< 900 divided by the frames shown per second; 0 plays at 30
    bool repeats = false;
    bool fixesPosition = false;
    std::uint32_t flags = 0;
    std::int32_t textureAnimationStart = -1; ///< the first of the set's texture animations that
                                             ///< this sequence keys to its frame; -1 for none
    std::int32_t textureAnimationCount = 0;
    std::vector<TrackInfo> tracks;
    std::vector<std::int32_t> trackOfNode; ///< per tree node: its track's index, -1 for none

    /** The keys node `node` plays in this sequence, or null when it stays at rest. */
    const TrackInfo* track(std::size_t node) const {
        return node < trackOfNode.size() && trackOfNode[node] >= 0
                   ? &tracks[static_cast<std::size_t>(trackOfNode[node])]
                   : nullptr;
    }
};

/**
 * A texture animation an archive plays on its level: frames cycled into one texture slot
 * (`source` the first frame's index in the same set, or the frame found by `frameName`
 * wherever it lives), or that slot's coordinates scrolled along u or v.
 */
struct TextureAnimationInfo {
    static constexpr std::int32_t kByName = -1;
    static constexpr std::int32_t kScrollU = -2;
    static constexpr std::int32_t kScrollV = -3;
    static constexpr std::int32_t kFreeRunning = -1; ///< the flag of one stepped on the game clock

    std::string name;
    std::string frameName;
    std::int32_t texture = -1; ///< the slot the animation plays into
    std::int32_t source = -1;
    std::int32_t frames = 0; ///< frames in the cycle; negative scrolls the other way
    std::int32_t start = 0;  ///< where in the cycle it begins
    std::int32_t rate = 0;   ///< game frames per step; 0 or 1 steps every frame
    std::int32_t offset =
        0; ///< the frame the cycle counts from: a keyed one's first sequence frame
    std::int32_t flag = kFreeRunning; ///< anything else is keyed to a sequence's frame by a tree

    bool scrolls() const { return source == kScrollU || source == kScrollV; }
    bool cycles() const { return source >= 0 || source == kByName; }
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
    Vec3 worldPosition(std::size_t node) const;
    std::optional<std::uint32_t> findSequence(std::string_view wanted) const;
    std::optional<std::uint32_t> findNode(std::string_view wanted) const;
};

/** The animation trees of one unpacked archive. */
class AnimationSet {
public:
    /** Reads `directory/animations.json`; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return !m_trees.empty(); }
    std::size_t size() const { return m_trees.size(); }
    const TreeInfo& tree(std::uint32_t index) const;
    std::optional<std::uint32_t> find(std::string_view name) const;
    const std::vector<TextureAnimationInfo>& textureAnimations() const {
        return m_textureAnimations;
    }
    const std::vector<ParticleTemplate>& particleTemplates() const { return m_particles; }

private:
    std::vector<TreeInfo> m_trees;
    std::vector<TextureAnimationInfo> m_textureAnimations;
    std::vector<ParticleTemplate> m_particles;
    std::unordered_map<std::string, std::uint32_t> m_byName;
};

} // namespace gdl
