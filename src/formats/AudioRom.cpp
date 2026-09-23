#include "formats/AudioRom.h"

#include <bit>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr usize kHeaderSize = 24;
constexpr usize kBankRecordSize = 44;
constexpr usize kSoundRecordSize = 28;
constexpr usize kNameSize = 16;

std::string readName(std::span<const u8> file, usize at) {
    std::string text;
    for (usize i = 0; i < kNameSize && file[at + i] != 0; ++i) {
        text.push_back(static_cast<char>(file[at + i]));
    }
    return text;
}

} // namespace

AudioRom AudioRom::parse(std::span<const u8> file) {
    if (file.size() < kHeaderSize) {
        throw FormatError("audio rom is too small for its header");
    }
    const u32 bankCount = readU32LE(file, 4);
    const u32 soundCount = readU32LE(file, 8);
    const u32 banksAt = readU32LE(file, 16);
    const u32 soundsAt = readU32LE(file, 20);
    if (banksAt > file.size() || usize{bankCount} * kBankRecordSize > file.size() - banksAt ||
        soundsAt > file.size() || usize{soundCount} * kSoundRecordSize > file.size() - soundsAt) {
        throw FormatError("audio rom tables lie outside the file");
    }

    AudioRom rom;
    for (u32 i = 0; i < bankCount; ++i) {
        const usize at = banksAt + usize{i} * kBankRecordSize;
        AudioRomBank bank;
        bank.file = readName(file, at);
        bank.name = normalizeAssetName(readName(file, at + kNameSize));
        bank.dataSize = readU32LE(file, at + 32);
        bank.soundCount = readU16LE(file, at + 36);
        bank.firstSound = readU16LE(file, at + 38);
        if (usize{bank.firstSound} + bank.soundCount > soundCount) {
            throw FormatError("audio rom bank refers to sounds outside the table");
        }
        rom.banks.push_back(std::move(bank));
    }
    for (u32 i = 0; i < soundCount; ++i) {
        const usize at = soundsAt + usize{i} * kSoundRecordSize;
        AudioRomSound sound;
        sound.name = readName(file, at);
        sound.id = readU32LE(file, at + 16);
        sound.duration = std::bit_cast<f32>(readU32LE(file, at + 20));
        rom.sounds.push_back(std::move(sound));
    }
    return rom;
}

std::optional<u32> AudioRom::findBank(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    for (u32 i = 0; i < banks.size(); ++i) {
        if (banks[i].name == key) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<u32> AudioRom::findSound(std::string_view name) const {
    for (u32 i = 0; i < sounds.size(); ++i) {
        if (sounds[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace gdl::formats
