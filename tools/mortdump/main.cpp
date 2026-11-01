#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "expansions/n64/audio/MortBank.h"
#include "formats/WavWriter.h"

namespace {
using namespace gdl;

void exportClip(const MortBank& bank, u16 id, const std::filesystem::path& directory,
                bool declick) {
    const auto source = declick ? bank.createRestored(id) : bank.create(id);
    if (!source) {
        throw std::runtime_error(std::format("Cannot decode MORT clip {}", id));
    }
    const auto format = source->desc();
    std::vector<s16> samples;
    std::vector<f32> block;
    for (;;) {
        block.clear();
        const bool more = source->read(block, 4096);
        if (block.empty() && more) {
            throw std::runtime_error("MORT decoder made no progress");
        }
        if (samples.size() + block.size() > usize{32'000} * 600) {
            throw std::runtime_error("MORT clip exceeds the export duration limit");
        }
        for (const auto sample : block) {
            if (!std::isfinite(sample)) {
                throw std::runtime_error("MORT decoder produced a non-finite sample");
            }
            samples.push_back(
                static_cast<s16>(std::clamp(std::lround(sample * 32768.0f), -32768L, 32767L)));
        }
        if (!more) {
            break;
        }
    }
    if (samples.empty()) {
        throw std::runtime_error("MORT clip has no samples");
    }
    const auto path = directory / std::format("speech-{:03}.wav", id);
    if (std::filesystem::exists(path)) {
        throw std::runtime_error("Refusing to overwrite " + paths::utf8(path));
    }
    writeFile(path, formats::encodeWav(samples, format.sampleRate, format.channels));
    std::printf("%s: %zu frames, %u Hz\n", paths::utf8(path.filename()).c_str(),
                samples.size() / format.channels, format.sampleRate);
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::puts(
            "Usage: mortdump <mort.mdir> <empty-output-directory> [clip-id] [--declick]\n"
            "Exports original N64 dialogue as PCM WAV files. Without clip-id, exports all clips.\n"
            "--declick applies the GameCube speech click/burst filter; default output is raw.");
        return 0;
    }
    if (argc < 3 || argc > 5) {
        std::fputs("Usage: mortdump <mort.mdir> <empty-output-directory> [clip-id] [--declick]\n",
                   stderr);
        return 2;
    }
    try {
        MortBank bank;
        if (!bank.open(paths::fromUtf8(argv[1]))) {
            throw std::runtime_error("Cannot open the native MORT dialogue bank");
        }
        usize first = 0;
        usize end = bank.size();
        bool declick = false;
        bool selectedClip = false;
        for (int arg = 3; arg < argc; ++arg) {
            const std::string_view text(argv[arg]);
            if (text == "--declick") {
                if (declick) {
                    throw std::runtime_error("--declick was specified more than once");
                }
                declick = true;
                continue;
            }
            if (selectedClip) {
                throw std::runtime_error("Only one clip ID may be selected");
            }
            const auto [last, error] =
                std::from_chars(text.data(), text.data() + text.size(), first);
            if (error != std::errc{} || last != text.data() + text.size() || first >= end) {
                throw std::runtime_error("Clip ID is outside this bank");
            }
            end = first + 1;
            selectedClip = true;
        }
        const auto output = paths::fromUtf8(argv[2]);
        if (std::filesystem::exists(output) &&
            (!std::filesystem::is_directory(output) || !std::filesystem::is_empty(output))) {
            throw std::runtime_error(
                "Choose a new or empty output directory; nothing was overwritten");
        }
        std::filesystem::create_directories(output);
        for (usize id = first; id < end; ++id) {
            exportClip(bank, static_cast<u16>(id), output, declick);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
