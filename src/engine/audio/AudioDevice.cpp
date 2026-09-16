#include "engine/audio/AudioDevice.h"

#include <span>

#include <miniaudio.h>

#include "engine/core/Log.h"

namespace gdl {

struct AudioDevice::Backend {
    ma_device device{};
};

namespace {

void dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount) {
    auto* mixer = static_cast<AudioMixer*>(device->pUserData);
    mixer->mix(
        std::span(static_cast<f32*>(output), usize{frameCount} * AudioMixer::kOutputChannels));
}

} // namespace

AudioDevice::AudioDevice() : m_mixer(kSampleRate), m_backend(std::make_unique<Backend>()) {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = AudioMixer::kOutputChannels;
    config.sampleRate = kSampleRate;
    config.dataCallback = dataCallback;
    config.pUserData = &m_mixer;

    if (ma_device_init(nullptr, &config, &m_backend->device) != MA_SUCCESS) {
        log::warn("No audio playback device; continuing without sound");
        m_backend.reset();
        return;
    }
    if (ma_device_start(&m_backend->device) != MA_SUCCESS) {
        log::warn("Audio device failed to start; continuing without sound");
        ma_device_uninit(&m_backend->device);
        m_backend.reset();
        return;
    }
    m_available = true;
    log::info("Audio: {} at {} Hz", static_cast<const char*>(m_backend->device.playback.name),
              kSampleRate);
}

AudioDevice::~AudioDevice() {
    if (m_backend) {
        ma_device_uninit(&m_backend->device);
    }
}

} // namespace gdl
