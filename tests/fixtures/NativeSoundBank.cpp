#include "fixtures/NativeSoundBank.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/codec/DspAdpcm.h"
#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/AudioRom.h"

namespace gdl::test {
namespace {

constexpr usize kNameSize = 16;
constexpr usize kRomHeaderSize = 24;
constexpr usize kBankRecordSize = 44;
constexpr usize kSampleNameAt = 32;
constexpr usize kDspHeaderSize = 96;
constexpr usize kDspCoefficientsAt = 28;
constexpr u16 kSampleMask = 0x0FFF;

void putBe16(ByteWriter& out, u16 value) {
    out.putU16(std::byteswap(value));
}

void putBe32(ByteWriter& out, u32 value) {
    out.putU32(std::byteswap(value));
}

void putName(ByteWriter& out, std::string_view name) {
    if (name.size() > kNameSize) {
        throw FormatError("synthetic audio name exceeds its native field");
    }
    out.putText(name).putZeros(kNameSize - name.size());
}

std::vector<u8> encodeSample(std::span<const s16> samples) {
    ByteWriter out;
    for (usize at = 0; at < samples.size(); at += DspAdpcmDecoder::kSamplesPerFrame) {
        const auto frame =
            samples.subspan(at, std::min(DspAdpcmDecoder::kSamplesPerFrame, samples.size() - at));
        u8 exponent = 0;
        for (const s16 sample : frame) {
            while (sample < -8 * (1 << exponent) || sample > 7 * (1 << exponent)) {
                ++exponent;
            }
        }
        const auto scale = static_cast<f32>(1 << exponent);
        out.putU8(exponent);
        const auto nibble = [&](usize index) -> u8 {
            const s32 value =
                index < frame.size()
                    ? static_cast<s32>(std::lround(static_cast<f32>(frame[index]) / scale))
                    : 0;
            return static_cast<u8>(std::clamp(value, -8, 7) & 15);
        };
        for (usize i = 0; i < DspAdpcmDecoder::kSamplesPerFrame; i += 2) {
            out.putU8(static_cast<u8>((nibble(i) << 4U) | nibble(i + 1)));
        }
    }
    return out.bytes();
}

void writeDirectory(const std::filesystem::path& path, const formats::AudioRom& directory) {
    ByteWriter out;
    out.putU32(0).putU32(static_cast<u32>(directory.banks.size()));
    out.putU32(static_cast<u32>(directory.sounds.size())).putU32(0);
    out.putU32(kRomHeaderSize);
    out.putU32(static_cast<u32>(kRomHeaderSize + directory.banks.size() * kBankRecordSize));
    for (const auto& bank : directory.banks) {
        putName(out, bank.file);
        putName(out, bank.name);
        out.putU32(bank.dataSize)
            .putU16(static_cast<u16>(bank.soundCount))
            .putU16(static_cast<u16>(bank.firstSound))
            .putU32(0);
    }
    for (const auto& sound : directory.sounds) {
        putName(out, sound.name);
        out.putU32(sound.id).putU32(std::bit_cast<u32>(sound.duration)).putU32(0);
    }
    writeFile(path, out.bytes());
}

} // namespace

void writeNativeSoundBank(const std::filesystem::path& bankPath, std::string_view soundsJson,
                          std::span<const NativeSoundSample> samples) {
    const auto declaration = nlohmann::json::parse(soundsJson);
    const auto& sounds = declaration.at("sounds");
    ByteWriter calls;
    for (const auto& sound : sounds) {
        const auto& sequence = sound.at("sequence");
        if (sequence.empty()) {
            throw FormatError("synthetic native calls require a sample step");
        }
        for (usize i = 0; i < sequence.size(); ++i) {
            const auto& step = sequence[i];
            const auto sample = step.at("sample").get<u16>();
            if (sample > kSampleMask || sample >= samples.size()) {
                throw FormatError("synthetic native call sample is out of range");
            }
            putBe16(calls, static_cast<u16>(sample | (step.value("loopStart", false) ? 0x4000 : 0) |
                                            (step.value("loopBack", false) ? 0x2000 : 0) |
                                            (i + 1 == sequence.size() ? 0x8000 : 0)));
        }
        putBe16(calls, sound.value("volume", u16{127}));
        putBe16(calls, sound.value("duck", u16{0}));
        putBe16(calls, sound.value("priority", u16{0}));
    }
    ByteWriter bank;
    bank.putText("KNBV");
    putBe32(bank, static_cast<u32>(calls.size()));
    putBe32(bank, 0);
    putBe32(bank, static_cast<u32>(sounds.size()));
    putBe32(bank, static_cast<u32>(samples.size()));
    bank.putBytes(calls.bytes());
    for (const auto& sample : samples) {
        if (sample.sampleRate == 0 || sample.samples.empty()) {
            throw FormatError("synthetic native sample requires PCM and a sample rate");
        }
        const auto adpcm = encodeSample(sample.samples);
        bank.putText("VAGp");
        putBe32(bank, 0x28);
        putBe32(bank, 0);
        putBe32(bank, static_cast<u32>(adpcm.size()));
        putBe32(bank, sample.sampleRate);
        bank.putZeros(kSampleNameAt - 20);
        putName(bank, "SAMPLE");
        putBe32(bank, static_cast<u32>(sample.samples.size()));
        putBe32(bank, 0);
        putBe32(bank, sample.sampleRate);
        bank.putZeros(kDspCoefficientsAt - 12);
        bank.putZeros(kDspHeaderSize - kDspCoefficientsAt);
        bank.putBytes(adpcm);
    }

    const bool explicitFile = toLowerAscii(bankPath.extension().string()) == ".vbk";
    const auto name =
        normalizeAssetName(explicitFile ? bankPath.stem().string() : bankPath.filename().string());
    const auto parent = bankPath.parent_path();
    std::filesystem::create_directories(parent);
    const auto romPath = parent / "AUDATPS2.ROM";
    formats::AudioRom directory;
    if (std::filesystem::exists(romPath)) {
        directory = formats::AudioRom::parse(readFile(romPath));
    }
    formats::AudioRom updated;
    for (auto& existing : directory.banks) {
        if (existing.name == name) {
            continue;
        }
        const auto begin = directory.sounds.begin() + existing.firstSound;
        const auto end = begin + existing.soundCount;
        existing.firstSound = static_cast<u16>(updated.sounds.size());
        updated.sounds.insert(updated.sounds.end(), begin, end);
        updated.banks.push_back(existing);
    }
    if (updated.sounds.size() + sounds.size() > std::numeric_limits<u16>::max()) {
        throw FormatError("synthetic audio directory exceeds native sound index range");
    }
    formats::AudioRomBank record;
    record.name = name;
    record.file = name;
    record.dataSize = static_cast<u32>(bank.size());
    record.soundCount = static_cast<u16>(sounds.size());
    record.firstSound = static_cast<u16>(updated.sounds.size());
    updated.banks.push_back(record);
    for (const auto& sound : sounds) {
        formats::AudioRomSound entry;
        entry.name = sound.at("name").get<std::string>();
        entry.id = sound.value("id", 0U);
        entry.duration = sound.value("duration", 0.0f);
        updated.sounds.push_back(entry);
    }
    writeDirectory(romPath, updated);
    writeFile(explicitFile ? bankPath : parent / (name + ".VBK"), bank.bytes());
}

} // namespace gdl::test
