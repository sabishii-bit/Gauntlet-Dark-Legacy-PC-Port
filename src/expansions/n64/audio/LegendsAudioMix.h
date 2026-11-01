#pragma once

#include <memory>
#include <optional>

#include "engine/audio/StreamSource.h"

namespace gdl::legendsAudioMix {

enum class Source { Music, Effect, Speech };

struct Calibration {
    Source kind = Source::Effect;
    f32 gain = 1;
    f32 activeRms = 0;
    f32 peak = 0;
    usize frames = 0;
    bool complete = false;
};

/** Measures a finite clip, or a bounded representative interval of a native
 * loop, then rewinds it. One constant gain preserves its envelopes and dynamics.
 * Effects must be measured at neutral cue volume/pan/pitch: authored controls
 * and spatial/user gain are applied to playback, never normalized away. */
std::optional<Calibration> calibrate(StreamSource& source, Source kind);

/** The common playback boundary for every native N64 music, macro and speech
 * source. Retains float headroom until the user's slider, resampling and the
 * shared final mixer limiter. No source clipping or duplicate compensation.
 * Reapplying the same profile returns the same source; conflicting profiles,
 * null sources and unsupported stream descriptions return null. A supplied
 * calibration lets repeated effects reuse a neutral reference measurement. */
std::shared_ptr<StreamSource> prepare(std::shared_ptr<StreamSource> source, Source kind,
                                      std::optional<Calibration> calibration = {});

} // namespace gdl::legendsAudioMix
