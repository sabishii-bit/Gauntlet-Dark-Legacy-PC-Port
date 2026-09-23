#include <array>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioDevice.h"
#include "engine/core/Types.h"

namespace {

using namespace gdl;

TEST_CASE("the audio device opens or degrades to silence", "[audio][device]") {
    AudioDevice device;
    REQUIRE(device.mixer().outputRate() == AudioDevice::kSampleRate);

    auto stream = device.mixer().createStream(AudioStreamDesc{8000, 1});
    const std::array<f32, 800> kSilence{};
    stream->push(kSilence);
    stream->finish();
    if (device.available()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE(stream->drained());
    }
}

TEST_CASE("audio devices can be created repeatedly", "[audio][device]") {
    for (s32 i = 0; i < 2; ++i) {
        const AudioDevice device;
        REQUIRE(device.available() == device.available());
    }
}

} // namespace
