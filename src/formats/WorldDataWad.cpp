#include "formats/WorldDataWad.h"

#include <format>

#include "engine/core/Error.h"
#include "formats/WadDirectory.h"

namespace gdl::formats {

namespace {

constexpr std::string_view kWhat = "world data wad";
constexpr usize kNameSize = 4;
constexpr usize kTextSize = 16;
constexpr usize kFogOffset = 0x70;

s16 readS16(std::span<const u8> bytes, usize at) {
    return static_cast<s16>(readWadU16(bytes, at, kWhat));
}

Vec3 readVec3(std::span<const u8> bytes, usize at) {
    return Vec3{readWadF32(bytes, at, kWhat), readWadF32(bytes, at + 4, kWhat),
                readWadF32(bytes, at + 8, kWhat)};
}

void requireRecords(std::span<const u8> bytes, const WadSection& section, usize size) {
    if (section.count > (bytes.size() - section.offset) / size) {
        throw FormatError(std::format("{}: {} records lie outside the file", kWhat, section.tag));
    }
}

LevelRecord readLevel(std::span<const u8> bytes, usize at) {
    LevelRecord level;
    level.flags = readWadU32(bytes, at, kWhat);
    level.name = readWadText(bytes, at + 8, kNameSize, kWhat);
    level.title = readWadText(bytes, at + 0x14, kTextSize, kWhat);
    level.audioBank = readWadText(bytes, at + 0x24, kTextSize, kWhat);
    level.movie = readWadText(bytes, at + 0x34, kTextSize, kWhat);
    level.bossType = static_cast<s32>(readWadU32(bytes, at + 0x44, kWhat));
    level.cameraIndex = readS16(bytes, at + 0x58);
    level.audioIndex = readS16(bytes, at + 0x5A);
    level.mapIndex = readS16(bytes, at + 0x5C);
    const usize fog = at + kFogOffset;
    level.fog.type = bytes[fog];
    level.fog.color = {bytes[fog + 1], bytes[fog + 2], bytes[fog + 3]};
    level.fog.intensity = readWadF32(bytes, fog + 4, kWhat);
    level.fog.density = readWadF32(bytes, fog + 8, kWhat);
    level.fog.min = readWadF32(bytes, fog + 12, kWhat);
    level.fog.max = readWadF32(bytes, fog + 16, kWhat);
    level.fog.near = readWadF32(bytes, fog + 20, kWhat);
    level.fog.far = readWadF32(bytes, fog + 24, kWhat);
    level.maxEnemies = readS16(bytes, at + 0x8E);
    level.rune = readS16(bytes, at + 0x90);
    level.legend = readS16(bytes, at + 0x92);
    level.musicVolume = readWadF32(bytes, at + 0x94, kWhat);
    level.soundVolume = readWadF32(bytes, at + 0x98, kWhat);
    for (usize i = 0; i < LevelTuningRecord::kCount; ++i) {
        level.tuning.values[i] = readWadF32(bytes, at + 0x9C + i * 4, kWhat);
    }
    level.ambient = readWadF32(bytes, at + 0xEC, kWhat);
    level.lightDirection = readVec3(bytes, at + 0xF0);
    level.lightColor = readVec3(bytes, at + 0xFC);
    level.lightIntensity = readWadF32(bytes, at + 0x108, kWhat);
    return level;
}

CameraRecord readCamera(std::span<const u8> bytes, usize at) {
    CameraRecord camera;
    camera.direction = readS16(bytes, at);
    camera.pitchDirection = readS16(bytes, at + 2);
    camera.dp = readWadF32(bytes, at + 4, kWhat);
    camera.minPitch = readWadF32(bytes, at + 8, kWhat);
    camera.boundsMin = readVec3(bytes, at + 0xC);
    camera.boundsMax = readVec3(bytes, at + 0x18);
    camera.limits = bytes[at + 0x24];
    camera.startEvent = bytes[at + 0x25];
    camera.attentionCamera = readS16(bytes, at + 0x26);
    camera.attention = readWadF32(bytes, at + 0x28, kWhat);
    camera.radiusMin = readWadF32(bytes, at + 0x2C, kWhat);
    camera.radiusMax = readWadF32(bytes, at + 0x30, kWhat);
    camera.enemyMax = readS16(bytes, at + 0x34);
    camera.specialRadius = readS16(bytes, at + 0x36);
    camera.maxPitch = readWadF32(bytes, at + 0x38, kWhat);
    camera.pitchSub = readWadF32(bytes, at + 0x3C, kWhat);
    camera.pitchMul = readWadF32(bytes, at + 0x40, kWhat);
    camera.pitchAdd = readWadF32(bytes, at + 0x44, kWhat);
    camera.distMulAdd = readWadF32(bytes, at + 0x48, kWhat);
    camera.distMulFactor = readWadF32(bytes, at + 0x4C, kWhat);
    camera.distMulMin = readWadF32(bytes, at + 0x50, kWhat);
    camera.distMulMax = readWadF32(bytes, at + 0x54, kWhat);
    camera.smooth = readWadF32(bytes, at + 0x58, kWhat);
    camera.minYaw = readWadF32(bytes, at + 0x5C, kWhat);
    camera.maxYaw = readWadF32(bytes, at + 0x60, kWhat);
    camera.bossRadiusMin = readWadF32(bytes, at + 0x64, kWhat);
    camera.bossRadiusMax = readWadF32(bytes, at + 0x68, kWhat);
    return camera;
}

AudioRecord readAudio(std::span<const u8> bytes, usize at) {
    AudioRecord audio;
    audio.bank = readWadText(bytes, at, kTextSize, kWhat);
    audio.enterSound = readS16(bytes, at + 0x10);
    audio.hitSound = readS16(bytes, at + 0x12);
    audio.nameSound = static_cast<s32>(readWadU32(bytes, at + 0x14, kWhat));
    audio.stream = readWadText(bytes, at + 0x18, kTextSize, kWhat);
    audio.areas = readS16(bytes, at + 0x28);
    audio.stereo = readS16(bytes, at + 0x2A);
    for (usize i = 0; i < audio.parts.size(); ++i) {
        audio.parts[i] = readS16(bytes, at + 0x2C + i * 2);
    }
    return audio;
}

SoundRecord readSound(std::span<const u8> bytes, usize at) {
    SoundRecord sound;
    sound.name = readWadText(bytes, at, kTextSize, kWhat);
    sound.volume = readS16(bytes, at + 0x14);
    sound.priority = readS16(bytes, at + 0x16);
    return sound;
}

} // namespace

WorldDataFile WorldDataFile::parse(std::span<const u8> bytes) {
    const std::vector<WadSection> sections = readWadDirectory(bytes, kWhat);
    WorldDataFile out;
    const WadSection* world = findWadSection(sections, "WRLD");
    if (world == nullptr) {
        throw FormatError(std::format("{}: no WRLD section", kWhat));
    }
    out.realm = readWadU32(bytes, world->offset, kWhat);
    out.prefix = readWadText(bytes, world->offset + 4, kTextSize, kWhat);
    if (const WadSection* levels = findWadSection(sections, "LEVL"); levels != nullptr) {
        requireRecords(bytes, *levels, kLevelSize);
        for (u32 i = 0; i < levels->count; ++i) {
            out.levels.push_back(readLevel(bytes, levels->offset + usize{i} * kLevelSize));
        }
    }
    if (const WadSection* cameras = findWadSection(sections, "CAMS"); cameras != nullptr) {
        requireRecords(bytes, *cameras, kCameraSize);
        for (u32 i = 0; i < cameras->count; ++i) {
            out.cameras.push_back(readCamera(bytes, cameras->offset + usize{i} * kCameraSize));
        }
    }
    if (const WadSection* audio = findWadSection(sections, "AUDS"); audio != nullptr) {
        requireRecords(bytes, *audio, kAudioSize);
        for (u32 i = 0; i < audio->count; ++i) {
            out.audio.push_back(readAudio(bytes, audio->offset + usize{i} * kAudioSize));
        }
    }
    if (const WadSection* sounds = findWadSection(sections, "SNDS"); sounds != nullptr) {
        requireRecords(bytes, *sounds, kSoundSize);
        for (u32 i = 0; i < sounds->count; ++i) {
            out.sounds.push_back(readSound(bytes, sounds->offset + usize{i} * kSoundSize));
        }
    }
    return out;
}

} // namespace gdl::formats
