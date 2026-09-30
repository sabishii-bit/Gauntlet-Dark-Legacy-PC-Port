#pragma once

#include <memory>
#include <vector>

#include "engine/audio/StreamSource.h"
#include "engine/core/Types.h"

namespace gdl {
/** Plays ordered stream parts once, then repeats the last part. All parts share a format.
 * A continuation handed to `follow` takes over where the part now playing ends. */
class StreamPlaylist final : public StreamSource {
public:
    explicit StreamPlaylist(std::vector<std::unique_ptr<StreamSource>> parts);
    AudioStreamDesc desc() const override;
    bool read(std::vector<f32>& out, usize frames) override;
    void rewind() override;

    /** Replaces these parts with `parts` once the part now playing (or repeating) ends;
     * an empty list cancels a continuation still waiting. They must share the format. */
    void follow(std::vector<std::unique_ptr<StreamSource>> parts);
    /** The part being read, from nought. */
    usize part() const { return m_current; }
    bool following() const { return !m_next.empty(); }
    /** How many continuations have taken over so far. */
    usize followed() const { return m_followed; }

private:
    static void verify(const std::vector<std::unique_ptr<StreamSource>>& parts,
                       const AudioStreamDesc& format);

    std::vector<std::unique_ptr<StreamSource>> m_parts;
    std::vector<std::unique_ptr<StreamSource>> m_next;
    usize m_current = 0;
    usize m_followed = 0;
};
} // namespace gdl
