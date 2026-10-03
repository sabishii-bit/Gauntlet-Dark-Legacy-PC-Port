#pragma once

#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/** Streaming impulse restoration. Unflagged samples pass unchanged; no gain or EQ is applied. */
class MusicDeclicker {
public:
    enum class Pass : u8 { Impulses, ShortBursts };

    void reset(u32 sampleRate, u32 channels, Pass pass = Pass::Impulses);
    void feed(std::span<const f32> samples, std::vector<f32>& output);
    void finish(std::vector<f32>& output);

private:
    void process(std::vector<f32>& output, bool finishing);
    void restore(std::span<const f64> input, std::span<f64> output) const;

    std::vector<f32> m_pending;
    usize m_channels = 0;
    usize m_window = 0;
    usize m_hop = 0;
    usize m_margin = 0;
    usize m_order = 0;
    usize m_burstLimit = 0;
    usize m_frames = 0;
    usize m_emitted = 0;
    Pass m_pass = Pass::Impulses;
    bool m_finished = false;
};

} // namespace gdl
