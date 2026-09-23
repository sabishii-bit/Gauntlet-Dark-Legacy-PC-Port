#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/audio/SoundClip.h"

namespace gdl {

struct SoundSetStep {
    std::uint32_t sample = 0;
    bool loopStart = false;
    bool loopBack = false;
};

struct SoundSetEntry {
    std::string name;
    std::uint32_t id = 0;
    float volume = 1.0f;   ///< 0..1
    float duration = 0.0f; ///< seconds; negative when it loops until stopped
    std::vector<SoundSetStep> sequence;
};

/** One unpacked sound bank: named sounds and the sample clips they sequence, decoded on demand. */
class SoundSet {
public:
    /** Reads `directory/sounds.json`; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return !m_entries.empty(); }
    std::size_t size() const { return m_entries.size(); }
    const SoundSetEntry& entry(std::uint32_t index) const;
    std::optional<std::uint32_t> find(std::string_view name) const;

    /** The decoded clip of a sample; throws FileError or FormatError when it cannot be read. */
    const SoundClip& sample(std::uint32_t index);

    /** The sound's clips in playing order, decoding what is missing; throws like sample(). */
    SoundSequence sequence(std::uint32_t index);

private:
    struct SampleInfo {
        std::filesystem::path file;
        SoundClip clip;
    };

    std::vector<SoundSetEntry> m_entries;
    std::unordered_map<std::string, std::uint32_t> m_byName;
    std::vector<SampleInfo> m_samples;
};

} // namespace gdl
