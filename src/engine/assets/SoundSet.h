#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/audio/SoundClip.h"
#include "engine/core/Types.h"

namespace gdl {

struct SoundSetStep {
    u32 sample = 0;
    bool loopStart = false;
    bool loopBack = false;
};

struct SoundSetEntry {
    std::string name;
    u32 id = 0;
    f32 volume = 1.0f;   ///< 0..1
    f32 duration = 0.0f; ///< seconds; negative when it loops until stopped
    std::vector<SoundSetStep> sequence;
};

/** One unpacked sound bank: named sounds and the sample clips they sequence, decoded on demand. */
class SoundSet {
public:
    /** Reads `directory/sounds.json`; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return !m_entries.empty(); }
    usize size() const { return m_entries.size(); }
    const SoundSetEntry& entry(u32 index) const;
    std::optional<u32> find(std::string_view name) const;

    /** The decoded clip of a sample; throws FileError or FormatError when it cannot be read. */
    const SoundClip& sample(u32 index);

    /** The sound's clips in playing order, decoding what is missing; throws like sample(). */
    SoundSequence sequence(u32 index);

private:
    struct SampleInfo {
        std::filesystem::path file;
        SoundClip clip;
    };

    std::vector<SoundSetEntry> m_entries;
    std::unordered_map<std::string, u32> m_byName;
    std::vector<SampleInfo> m_samples;
};

} // namespace gdl
