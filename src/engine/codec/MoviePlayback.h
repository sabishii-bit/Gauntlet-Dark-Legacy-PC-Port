#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "engine/codec/AdsAudio.h"
#include "engine/codec/AviReader.h"
#include "engine/codec/VqVideoDecoder.h"
#include "engine/render/Image.h"

namespace gdl {

struct MovieInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double framesPerSecond = 0.0;
    std::uint32_t frameCount = 0;
    bool hasAudio = false;
    bool audioReady = false; ///< sample rate and channel count are final
    std::uint32_t audioSampleRate = 0;
    std::uint32_t audioChannels = 0;
};

/**
 * Drives a VQ movie in real time without touching the GPU or the sound card: demuxes the
 * file, decodes the frames that are due, and hands out audio samples as they arrive.
 */
class MoviePlayback {
public:
    /** Opens a movie file; logs and returns false when it cannot be played. */
    bool open(const std::filesystem::path& path);
    void close();
    bool isOpen() const { return m_reader != nullptr; }

    const MovieInfo& info() const { return m_info; }

    /** Advances by `deltaSeconds`; returns false once the movie is over. */
    bool update(double deltaSeconds);

    /** True when the last update() decoded at least one new frame. */
    bool frameChanged() const { return m_frameChanged; }
    const Image& frame() const;
    std::uint32_t decodedFrames() const { return m_decodedFrames; }
    double playTime() const { return m_playTime; }

    /** Appends the interleaved float samples decoded since the last call and forgets them. */
    void takeAudio(std::vector<float>& out);

private:
    enum class AudioCoding : std::uint8_t { Unknown, Pcm, Ads };

    void fillLookahead();
    bool demuxNext();
    void queueAudio(std::span<const std::uint8_t> data);
    void queuePcm(std::span<const std::uint8_t> data);
    void queueAds(std::span<const std::uint8_t> data);
    void countQueuedAudio(std::size_t samplesBefore);

    std::unique_ptr<AviReader> m_reader;
    std::unique_ptr<VqVideoDecoder> m_decoder;
    MovieInfo m_info;
    AviAudioFormat m_audioFormat;
    AudioCoding m_audioCoding = AudioCoding::Unknown;
    AdsAudioDecoder m_ads;
    std::vector<std::uint8_t> m_adsHeaderBytes;
    std::uint32_t m_videoStream = 0;
    std::uint32_t m_audioStream = 0;
    std::deque<std::vector<std::uint8_t>> m_pendingVideo;
    std::vector<float> m_audio;
    double m_audioQueuedSeconds = 0.0;
    double m_playTime = 0.0;
    std::uint32_t m_decodedFrames = 0;
    bool m_frameChanged = false;
    bool m_endOfStream = false;
    bool m_reportedDecodeError = false;
};

} // namespace gdl
