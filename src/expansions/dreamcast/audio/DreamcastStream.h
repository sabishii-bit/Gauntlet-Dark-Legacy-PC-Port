#pragma once

#include <array>
#include <filesystem>
#include <fstream>

#include "engine/audio/StreamSource.h"

namespace gdl {

/** Native Sega STR / AICA ADPCM; bounded streaming with no converted WAV dependency. */
class DreamcastStream final : public StreamSource {
public:
    bool open(const std::filesystem::path& path);
    bool opened() const { return m_desc.channels != 0; }
    u64 frames() const { return m_frames; }
    f64 seconds() const;
    AudioStreamDesc desc() const override { return m_desc; }
    bool read(std::vector<f32>& out, usize frames) override;
    void rewind() override;

private:
    struct Channel {
        s32 sample = 0;
        s32 step = 127;
    };
    std::ifstream m_file;
    AudioStreamDesc m_desc{0, 0};
    std::array<Channel, 2> m_channels{};
    std::vector<u8> m_block;
    usize m_interleave = 0;
    usize m_blockFrame = 0;
    u64 m_frames = 0;
    u64 m_frame = 0;
};

} // namespace gdl
