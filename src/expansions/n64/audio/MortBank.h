#pragma once

#include <filesystem>
#include <memory>
#include <span>

#include "engine/audio/StreamSource.h"

namespace gdl {

/** Optional native N64 speech directory. Each created voice has independent decoder state. */
class MortBank {
public:
    bool open(const std::filesystem::path& path);
    bool open(std::span<const u8> bytes);
    bool opened() const { return static_cast<bool>(m_data); }
    usize size() const;
    bool contains(u16 id) const;
    /** Unmodified native PCM, for extraction and decoder parity checks. */
    std::shared_ptr<StreamSource> create(u16 id) const;
    /** Concatenates concrete clips; all must have the same authored sample rate. */
    std::shared_ptr<StreamSource> create(std::span<const u16> ids) const;
    /** Playback adapter using the same click/burst restoration as GameCube speech.
     * Each clip is restored independently, before resampling, without changing its duration. */
    std::shared_ptr<StreamSource> createRestored(u16 id) const;
    std::shared_ptr<StreamSource> createRestored(std::span<const u16> ids) const;

private:
    struct Data;
    std::shared_ptr<const Data> m_data;
};

} // namespace gdl
