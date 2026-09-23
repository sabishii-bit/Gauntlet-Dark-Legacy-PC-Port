#include "formats/AudioRom.h"

#include <bit>
#include <cstddef>
#include <cstdint>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kBankRecordSize = 44;
constexpr std::size_t kSoundRecordSize = 28;
constexpr std::size_t kNameSize = 16;

std::string readName(std::span<const std::uint8_t> file, std::size_t at) {
    std::string text;
    for (std::size_t i = 0; i < kNameSize && file[at + i] != 0; ++i) {
        text.push_back(static_cast<char>(file[at + i]));
    }
    return text;
}

} // namespace

AudioRom AudioRom::parse(std::span<const std::uint8_t> file) {
    if (file.size() < kHeaderSize) {
        throw FormatError("audio rom is too small for its header");
    }
    const std::uint32_t bankCount = readU32LE(file, 4);
    const std::uint32_t soundCount = readU32LE(file, 8);
    const std::uint32_t banksAt = readU32LE(file, 16);
    const std::uint32_t soundsAt = readU32LE(file, 20);
    if (banksAt > file.size() || std::size_t{bankCount} * kBankRecordSize > file.size() - banksAt ||
        soundsAt > file.size() ||
        std::size_t{soundCount} * kSoundRecordSize > file.size() - soundsAt) {
        throw FormatError("audio rom tables lie outside the file");
    }

    AudioRom rom;
    for (std::uint32_t i = 0; i < bankCount; ++i) {
        const std::size_t at = banksAt + std::size_t{i} * kBankRecordSize;
        AudioRomBank bank;
        bank.file = readName(file, at);
        bank.name = normalizeAssetName(readName(file, at + kNameSize));
        bank.dataSize = readU32LE(file, at + 32);
        bank.soundCount = readU16LE(file, at + 36);
        bank.firstSound = readU16LE(file, at + 38);
        if (std::size_t{bank.firstSound} + bank.soundCount > soundCount) {
            throw FormatError("audio rom bank refers to sounds outside the table");
        }
        rom.banks.push_back(std::move(bank));
    }
    for (std::uint32_t i = 0; i < soundCount; ++i) {
        const std::size_t at = soundsAt + std::size_t{i} * kSoundRecordSize;
        AudioRomSound sound;
        sound.name = readName(file, at);
        sound.id = readU32LE(file, at + 16);
        sound.duration = std::bit_cast<float>(readU32LE(file, at + 20));
        rom.sounds.push_back(std::move(sound));
    }
    return rom;
}

std::optional<std::uint32_t> AudioRom::findBank(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    for (std::uint32_t i = 0; i < banks.size(); ++i) {
        if (banks[i].name == key) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> AudioRom::findSound(std::string_view name) const {
    for (std::uint32_t i = 0; i < sounds.size(); ++i) {
        if (sounds[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::formats
