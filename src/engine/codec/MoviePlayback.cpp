#include "engine/codec/MoviePlayback.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/ByteReader.h"

namespace gdl {

namespace {

constexpr std::uint32_t kVideoStreamType = fourcc("vids");
constexpr std::uint32_t kAudioStreamType = fourcc("auds");
constexpr std::uint32_t kVqCodec = fourcc("MVDV");
constexpr std::uint16_t kPcmFormat = 1;
constexpr double kMaxStepSeconds = 0.25;
constexpr std::uint32_t kMaxFramesPerUpdate = 8;
constexpr std::size_t kVideoLookahead = 2;
constexpr double kAudioLookaheadSeconds = 1.0;
constexpr double kMicrosecondsPerSecond = 1'000'000.0;

} // namespace

bool MoviePlayback::open(const std::filesystem::path& path) {
    close();
    try {
        m_reader = std::make_unique<AviReader>(path);
        const AviHeader& header = m_reader->header();
        const AviStream* video = nullptr;
        const AviStream* audio = nullptr;
        for (std::uint32_t i = 0; i < header.streams.size(); ++i) {
            const AviStream& stream = header.streams[i];
            if (stream.type == kVideoStreamType && video == nullptr) {
                video = &stream;
                m_videoStream = i;
            } else if (stream.type == kAudioStreamType && audio == nullptr) {
                audio = &stream;
                m_audioStream = i;
            }
        }
        if (video == nullptr || !video->video.has_value()) {
            throw FormatError("movie has no video stream");
        }
        if (video->video->compression != kVqCodec) {
            throw FormatError("movie video is not MVDV");
        }

        m_info = MovieInfo{};
        m_info.width = video->video->width;
        m_info.height = static_cast<std::uint32_t>(std::abs(video->video->height));
        m_info.frameCount = video->length != 0 ? video->length : header.totalFrames;
        if (video->scale != 0 && video->rate != 0) {
            m_info.framesPerSecond = static_cast<double>(video->rate) / video->scale;
        } else if (header.microSecondsPerFrame != 0) {
            m_info.framesPerSecond = kMicrosecondsPerSecond / header.microSecondsPerFrame;
        } else {
            throw FormatError("movie has no frame rate");
        }
        if (audio != nullptr && audio->audio.has_value() && audio->audio->formatTag == kPcmFormat &&
            audio->audio->channels > 0) {
            m_audioFormat = *audio->audio;
            m_info.hasAudio = true;
        }
        m_decoder = std::make_unique<VqVideoDecoder>(m_info.width, m_info.height);
    } catch (const std::exception& e) {
        log::warn("Cannot play {}: {}", path.string(), e.what());
        close();
        return false;
    }

    fillLookahead();
    const char* audioNote = "";
    if (m_info.audioReady) {
        audioNote = m_audioCoding == AudioCoding::Ads ? ", ADPCM audio" : ", PCM audio";
    }
    log::info("Movie {}: {}x{} @ {:.2f} fps, {} frames{}", path.filename().string(), m_info.width,
              m_info.height, m_info.framesPerSecond, m_info.frameCount, audioNote);
    return true;
}

void MoviePlayback::close() {
    m_reader.reset();
    m_decoder.reset();
    m_info = MovieInfo{};
    m_audioFormat = AviAudioFormat{};
    m_audioCoding = AudioCoding::Unknown;
    m_ads = AdsAudioDecoder{};
    m_adsHeaderBytes.clear();
    m_pendingVideo.clear();
    m_audio.clear();
    m_audioQueuedSeconds = 0.0;
    m_playTime = 0.0;
    m_decodedFrames = 0;
    m_frameChanged = false;
    m_endOfStream = false;
    m_reportedDecodeError = false;
}

const Image& MoviePlayback::frame() const {
    static const Image kEmpty;
    return m_decoder ? m_decoder->frame() : kEmpty;
}

bool MoviePlayback::update(double deltaSeconds) {
    m_frameChanged = false;
    if (!isOpen()) {
        return false;
    }
    m_playTime += std::clamp(deltaSeconds, 0.0, kMaxStepSeconds);

    const std::uint32_t due = std::min(
        static_cast<std::uint32_t>(m_playTime * m_info.framesPerSecond), m_info.frameCount);
    std::uint32_t steps = 0;
    while (m_decodedFrames < due && steps < kMaxFramesPerUpdate) {
        fillLookahead();
        if (m_pendingVideo.empty()) {
            break;
        }
        const std::vector<std::uint8_t> chunk = std::move(m_pendingVideo.front());
        m_pendingVideo.pop_front();
        try {
            m_decoder->decode(chunk);
        } catch (const FormatError& e) {
            if (!m_reportedDecodeError) {
                log::warn("Movie frame {} failed to decode: {}", m_decodedFrames, e.what());
                m_reportedDecodeError = true;
            }
        }
        ++m_decodedFrames;
        m_frameChanged = true;
        ++steps;
    }
    fillLookahead();

    const bool noMoreFrames =
        m_decodedFrames >= m_info.frameCount || (m_endOfStream && m_pendingVideo.empty());
    const double dueFrames = m_playTime * m_info.framesPerSecond;
    return !noMoreFrames || dueFrames < static_cast<double>(m_decodedFrames);
}

void MoviePlayback::takeAudio(std::vector<float>& out) {
    out.insert(out.end(), m_audio.begin(), m_audio.end());
    m_audio.clear();
}

void MoviePlayback::fillLookahead() {
    while (!m_endOfStream &&
           (m_pendingVideo.size() < kVideoLookahead ||
            (m_info.hasAudio && m_audioQueuedSeconds < m_playTime + kAudioLookaheadSeconds))) {
        if (!demuxNext()) {
            m_endOfStream = true;
            if (m_audioCoding == AudioCoding::Ads) {
                const std::size_t before = m_audio.size();
                m_ads.flush(m_audio);
                countQueuedAudio(before);
            }
        }
    }
}

bool MoviePlayback::demuxNext() {
    try {
        auto chunk = m_reader->next();
        if (!chunk.has_value()) {
            return false;
        }
        if (chunk->kind == AviChunkKind::Video && chunk->stream == m_videoStream) {
            m_pendingVideo.push_back(std::move(chunk->data));
        } else if (chunk->kind == AviChunkKind::Audio && chunk->stream == m_audioStream &&
                   m_info.hasAudio) {
            queueAudio(chunk->data);
        }
        return true;
    } catch (const std::exception& e) {
        log::warn("Movie stream ended early: {}", e.what());
        return false;
    }
}

void MoviePlayback::queueAudio(std::span<const std::uint8_t> data) {
    if (m_audioCoding == AudioCoding::Unknown) {
        m_audioCoding = AdsAudioDecoder::looksLikeAds(data) ? AudioCoding::Ads : AudioCoding::Pcm;
        if (m_audioCoding == AudioCoding::Pcm) {
            m_info.audioSampleRate = m_audioFormat.samplesPerSecond;
            m_info.audioChannels = m_audioFormat.channels;
            m_info.audioReady = true;
        }
    }
    const std::size_t before = m_audio.size();
    if (m_audioCoding == AudioCoding::Ads) {
        queueAds(data);
    } else {
        queuePcm(data);
    }
    countQueuedAudio(before);
}

void MoviePlayback::queuePcm(std::span<const std::uint8_t> data) {
    constexpr float kUnsigned8Scale = 1.0f / 128.0f;
    constexpr float kSigned16Scale = 1.0f / 32768.0f;
    constexpr int kUnsigned8Bias = 128;

    if (m_audioFormat.bitsPerSample == 8) {
        m_audio.reserve(m_audio.size() + data.size());
        for (const std::uint8_t sample : data) {
            m_audio.push_back(static_cast<float>(int{sample} - kUnsigned8Bias) * kUnsigned8Scale);
        }
    } else if (m_audioFormat.bitsPerSample == 16) {
        const std::size_t samples = data.size() / 2;
        m_audio.reserve(m_audio.size() + samples);
        for (std::size_t i = 0; i < samples; ++i) {
            const auto sample = static_cast<std::int16_t>(readU16LE(data, i * 2));
            m_audio.push_back(static_cast<float>(sample) * kSigned16Scale);
        }
    }
}

void MoviePlayback::queueAds(std::span<const std::uint8_t> data) {
    if (!m_ads.hasHeader()) {
        m_adsHeaderBytes.insert(m_adsHeaderBytes.end(), data.begin(), data.end());
        const auto used = m_ads.parseHeader(m_adsHeaderBytes);
        if (!used.has_value()) {
            return;
        }
        m_info.audioSampleRate = m_ads.info().sampleRate;
        m_info.audioChannels = m_ads.info().channels;
        m_info.audioReady = true;
        const std::vector<std::uint8_t> rest(
            m_adsHeaderBytes.begin() + static_cast<std::ptrdiff_t>(*used), m_adsHeaderBytes.end());
        m_adsHeaderBytes.clear();
        m_ads.feed(rest, m_audio);
        return;
    }
    m_ads.feed(data, m_audio);
}

void MoviePlayback::countQueuedAudio(std::size_t samplesBefore) {
    if (!m_info.audioReady || m_info.audioSampleRate == 0 || m_info.audioChannels == 0) {
        return;
    }
    const std::size_t frames = (m_audio.size() - samplesBefore) / m_info.audioChannels;
    m_audioQueuedSeconds += static_cast<double>(frames) / m_info.audioSampleRate;
}

} // namespace gdl
