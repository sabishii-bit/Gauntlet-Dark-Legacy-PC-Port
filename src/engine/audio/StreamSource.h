#pragma once

#include <cstddef>
#include <vector>

#include "engine/audio/AudioStream.h"

namespace gdl {

/** Audio decoded a piece at a time as a voice needs it, such as a music stream on disk. */
class StreamSource {
public:
    virtual ~StreamSource() = default;

    virtual AudioStreamDesc desc() const = 0;
    /** Appends about `frames` decoded interleaved frames to `out`; false once nothing is
     * left. */
    virtual bool read(std::vector<float>& out, std::size_t frames) = 0;
    /** Starts over from the beginning. */
    virtual void rewind() = 0;
};

} // namespace gdl
