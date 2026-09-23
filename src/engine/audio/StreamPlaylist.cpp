#include "engine/audio/StreamPlaylist.h"

#include <stdexcept>
#include <utility>

namespace gdl {
StreamPlaylist::StreamPlaylist(std::vector<std::unique_ptr<StreamSource>> parts)
    : m_parts(std::move(parts)) {
    if (m_parts.empty() || m_parts.front() == nullptr) {
        throw std::invalid_argument("a stream playlist needs at least one part");
    }
    const auto format = m_parts.front()->desc();
    for (const auto& part : m_parts) {
        if (part == nullptr || part->desc().channels != format.channels ||
            part->desc().sampleRate != format.sampleRate) {
            throw std::invalid_argument("stream playlist parts must share a format");
        }
    }
}

AudioStreamDesc StreamPlaylist::desc() const {
    return m_parts.front()->desc();
}

bool StreamPlaylist::read(std::vector<f32>& out, usize frames) {
    while (!m_parts[m_current]->read(out, frames)) {
        if (m_current + 1 < m_parts.size()) {
            ++m_current;
        } else {
            m_parts[m_current]->rewind();
            // An empty final part must terminate, not spin forever.
            return m_parts[m_current]->read(out, frames);
        }
    }
    return true;
}

void StreamPlaylist::rewind() {
    for (auto& part : m_parts) {
        part->rewind();
    }
    m_current = 0;
}
} // namespace gdl
