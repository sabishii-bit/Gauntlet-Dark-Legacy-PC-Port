#pragma once

#include <memory>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"

namespace gdl {

/** Opens the default playback device and pulls from an AudioMixer on the device thread. */
class AudioDevice {
public:
    static constexpr u32 kSampleRate = 48000;

    AudioDevice();
    ~AudioDevice();

    GDL_NON_COPYABLE_NON_MOVABLE(AudioDevice);

    /** False when no playback device could be opened; the mixer still works, silently. */
    bool available() const { return m_available; }
    AudioMixer& mixer() { return m_mixer; }

private:
    struct Backend;

    AudioMixer m_mixer;
    std::unique_ptr<Backend> m_backend;
    bool m_available = false;
};

} // namespace gdl
