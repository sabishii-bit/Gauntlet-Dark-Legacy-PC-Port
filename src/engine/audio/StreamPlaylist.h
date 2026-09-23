#pragma once

#include <memory>
#include <vector>

#include "engine/audio/StreamSource.h"
#include "engine/core/Types.h"

namespace gdl {
/** Plays ordered stream parts once, then repeats the last part. All parts share a format. */
class StreamPlaylist final : public StreamSource {
public:
    explicit StreamPlaylist(std::vector<std::unique_ptr<StreamSource>> parts);
    AudioStreamDesc desc() const override;
    bool read(std::vector<f32>& out, usize frames) override;
    void rewind() override;

private:
    std::vector<std::unique_ptr<StreamSource>> m_parts;
    usize m_current = 0;
};
} // namespace gdl
