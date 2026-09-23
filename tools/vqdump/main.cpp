#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <stb_image_write.h>

#include "engine/codec/AdsAudio.h"
#include "engine/codec/AviReader.h"
#include "engine/codec/VqVideoDecoder.h"
#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

namespace {

using namespace gdl;

struct Options {
    std::filesystem::path movie;
    std::filesystem::path outputDirectory;
    std::uint32_t maxFrames = 0; ///< 0 = every frame
    std::uint32_t every = 1;     ///< write every n-th frame
};

void print(std::string_view text) {
    std::fputs(text.data(), stdout);
    std::fputc('\n', stdout);
}

std::optional<std::uint32_t> parseNumber(std::string_view text) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<Options> parseOptions(std::span<const std::string_view> args) {
    Options options;
    std::vector<std::string_view> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const bool hasValue = i + 1 < args.size();
        if ((arg == "--max-frames" || arg == "--every") && hasValue) {
            const auto value = parseNumber(args[++i]);
            if (!value.has_value()) {
                return std::nullopt;
            }
            (arg == "--every" ? options.every : options.maxFrames) = *value;
        } else if (arg.starts_with("--")) {
            return std::nullopt;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 2 || options.every == 0) {
        return std::nullopt;
    }
    options.movie = positional[0];
    options.outputDirectory = positional[1];
    return options;
}

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    appendU16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
    appendU16(out, static_cast<std::uint16_t>(value >> 16U));
}

void appendFourcc(std::vector<std::uint8_t>& out, std::string_view code) {
    appendU32(out, fourcc(code));
}

/** Writes 16-bit PCM WAV from interleaved float samples. */
void writeWav(const std::filesystem::path& path, std::uint32_t sampleRate, std::uint32_t channels,
              std::span<const float> samples) {
    constexpr std::uint32_t kFormatChunkSize = 16;
    constexpr std::uint16_t kPcm = 1;
    constexpr std::uint16_t kBitsPerSample = 16;
    constexpr float kScale = 32767.0f;
    const auto blockAlign = static_cast<std::uint16_t>(channels * 2);

    std::vector<std::uint8_t> wav;
    appendFourcc(wav, "RIFF");
    appendU32(wav, static_cast<std::uint32_t>(36 + samples.size() * 2));
    appendFourcc(wav, "WAVE");
    appendFourcc(wav, "fmt ");
    appendU32(wav, kFormatChunkSize);
    appendU16(wav, kPcm);
    appendU16(wav, static_cast<std::uint16_t>(channels));
    appendU32(wav, sampleRate);
    appendU32(wav, sampleRate * blockAlign);
    appendU16(wav, blockAlign);
    appendU16(wav, kBitsPerSample);
    appendFourcc(wav, "data");
    appendU32(wav, static_cast<std::uint32_t>(samples.size() * 2));
    for (const float sample : samples) {
        appendU16(wav, static_cast<std::uint16_t>(
                           static_cast<std::int16_t>(std::clamp(sample, -1.0f, 1.0f) * kScale)));
    }
    writeFile(path, wav);
}

/** Converts the raw audio track to float samples, decoding the ADS wrapper when present. */
std::vector<float> decodeAudio(const AviAudioFormat& format, std::span<const std::uint8_t> raw,
                               std::uint32_t& sampleRate, std::uint32_t& channels) {
    std::vector<float> samples;
    if (AdsAudioDecoder::looksLikeAds(raw)) {
        AdsAudioDecoder decoder;
        const auto used = decoder.parseHeader(raw);
        if (!used.has_value()) {
            throw FormatError("ADS audio header is truncated");
        }
        decoder.feed(raw.subspan(*used), samples);
        decoder.flush(samples);
        sampleRate = decoder.info().sampleRate;
        channels = decoder.info().channels;
        return samples;
    }
    sampleRate = format.samplesPerSecond;
    channels = format.channels;
    samples.reserve(raw.size());
    for (const std::uint8_t byte : raw) {
        samples.push_back(static_cast<float>(int{byte} - 128) / 128.0f);
    }
    return samples;
}

int run(const Options& options) {
    AviReader reader(options.movie);
    const AviHeader& header = reader.header();
    const AviStream* video = nullptr;
    const AviStream* audio = nullptr;
    std::uint32_t videoStream = 0;
    std::uint32_t audioStream = 0;
    for (std::uint32_t i = 0; i < header.streams.size(); ++i) {
        const AviStream& stream = header.streams[i];
        if (stream.type == fourcc("vids") && video == nullptr) {
            video = &stream;
            videoStream = i;
        } else if (stream.type == fourcc("auds") && audio == nullptr) {
            audio = &stream;
            audioStream = i;
        }
    }
    if (video == nullptr || !video->video.has_value()) {
        print("no video stream");
        return 1;
    }
    const AviVideoFormat& format = *video->video;
    const std::uint32_t width = format.width;
    const std::uint32_t height =
        static_cast<std::uint32_t>(format.height < 0 ? -format.height : format.height);
    print(std::format("video: {}x{} {} bpp, {} frames, {} fps", width, height, format.bitCount,
                      video->length, video->scale != 0 ? video->rate / video->scale : 0));

    std::filesystem::create_directories(options.outputDirectory);
    VqVideoDecoder decoder(width, height);
    std::vector<std::uint8_t> audioBytes;
    std::uint32_t frames = 0;
    std::uint32_t written = 0;
    std::uint32_t failures = 0;
    while (auto chunk = reader.next()) {
        if (chunk->kind == AviChunkKind::Audio && chunk->stream == audioStream) {
            audioBytes.insert(audioBytes.end(), chunk->data.begin(), chunk->data.end());
            continue;
        }
        if (chunk->kind != AviChunkKind::Video || chunk->stream != videoStream) {
            continue;
        }
        try {
            decoder.decode(chunk->data);
        } catch (const FormatError& e) {
            ++failures;
            print(std::format("frame {}: {}", frames, e.what()));
        }
        if (frames % options.every == 0 &&
            (options.maxFrames == 0 || written < options.maxFrames)) {
            const Image& image = decoder.frame();
            const std::string name =
                (options.outputDirectory / std::format("frame_{:04}.png", frames)).string();
            if (stbi_write_png(name.c_str(), static_cast<int>(image.width),
                               static_cast<int>(image.height), 4, image.pixels.data(),
                               static_cast<int>(image.rowBytes())) == 0) {
                print(std::format("cannot write {}", name));
                return 1;
            }
            ++written;
        }
        ++frames;
    }

    if (audio != nullptr && audio->audio.has_value() && !audioBytes.empty()) {
        std::uint32_t sampleRate = 0;
        std::uint32_t channels = 0;
        const std::vector<float> samples =
            decodeAudio(*audio->audio, audioBytes, sampleRate, channels);
        writeWav(options.outputDirectory / "audio.wav", sampleRate, channels, samples);
        print(std::format("audio: {} Hz, {} channel(s), {:.2f} s{}", sampleRate, channels,
                          static_cast<double>(samples.size()) / channels / sampleRate,
                          AdsAudioDecoder::looksLikeAds(audioBytes) ? " (ADS ADPCM)" : " (PCM)"));
    }
    print(
        std::format("{} frames decoded ({} failed), {} images written", frames, failures, written));
    return failures == 0 ? 0 : 3;
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string_view> args;
    const std::span<char*> rawArgs(argv, static_cast<std::size_t>(argc));
    for (const char* arg : rawArgs.subspan(rawArgs.empty() ? 0 : 1)) {
        args.emplace_back(arg);
    }
    const auto options = parseOptions(args);
    if (!options.has_value()) {
        print("usage: vqdump <movie.avi> <output-dir> [--max-frames n] [--every n]");
        return 2;
    }
    try {
        return run(*options);
    } catch (const std::exception& e) {
        print(std::format("error: {}", e.what()));
        return 1;
    }
}
