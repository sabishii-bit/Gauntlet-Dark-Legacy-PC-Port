#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "engine/codec/AdsAudio.h"
#include "engine/codec/AviReader.h"
#include "engine/codec/VqVideoDecoder.h"
#include "engine/core/Types.h"
#include "engine/render/Image.h"

namespace gdl {

struct MovieInfo {
    u32 width = 0;
    u32 height = 0;
    f64 framesPerSecond = 0.0;
    u32 frameCount = 0;
    bool hasAudio = false;
    bool audioReady = false; ///< sample rate and channel count are final
    u32 audioSampleRate = 0;
    u32 audioChannels = 0;
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
    bool update(f64 deltaSeconds);

    /** True when the last update() decoded at least one new frame. */
    bool frameChanged() const { return m_frameChanged; }
    const Image& frame() const;
    u32 decodedFrames() const { return m_decodedFrames; }
    f64 playTime() const { return m_playTime; }

    /** Appends the interleaved float samples decoded since the last call and forgets them. */
    void takeAudio(std::vector<f32>& out);

private:
    enum class AudioCoding : u8 { Unknown, Pcm, Ads };

    void fillLookahead();
    bool demuxNext();
    void queueAudio(std::span<const u8> data);
    void queuePcm(std::span<const u8> data);
    void queueAds(std::span<const u8> data);
    void countQueuedAudio(usize samplesBefore);

    std::unique_ptr<AviReader> m_reader;
    std::unique_ptr<VqVideoDecoder> m_decoder;
    MovieInfo m_info;
    AviAudioFormat m_audioFormat;
    AudioCoding m_audioCoding = AudioCoding::Unknown;
    AdsAudioDecoder m_ads;
    std::vector<u8> m_adsHeaderBytes;
    u32 m_videoStream = 0;
    u32 m_audioStream = 0;
    std::deque<std::vector<u8>> m_pendingVideo;
    std::vector<f32> m_audio;
    f64 m_audioQueuedSeconds = 0.0;
    f64 m_playTime = 0.0;
    u32 m_decodedFrames = 0;
    bool m_frameChanged = false;
    bool m_endOfStream = false;
    bool m_reportedDecodeError = false;
};

} // namespace gdl
