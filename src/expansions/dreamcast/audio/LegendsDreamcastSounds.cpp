#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

namespace gdl::game {

bool LegendsDreamcastSounds::open(const std::filesystem::path& nativeRoot) {
    m_banks.clear();
    m_directoryTable = {};
    m_directory = nativeRoot.parent_path() / "disc/AUDIO";
    try {
        const auto file = AssetLocator{m_directory}.find("aud_data.rom");
        if (!file)
            return false;
        m_directoryTable = formats::AudioRom::parse(readFile(*file));
        return !m_directoryTable.banks.empty();
    } catch (const std::exception& error) {
        log::warn("Dreamcast sound directory: {}", error.what());
        return false;
    }
}

std::optional<u32> LegendsDreamcastSounds::namedToken(std::string_view name) const {
    // Native sound lookup uses the bank directory's 15-character keys. Many
    // generated enemy names (for example S_TROLL2HIT1CLOSE) exceed that length.
    const auto entry = m_directoryTable.findSound(name.substr(0, 15));
    return entry ? std::optional{m_directoryTable.sounds[*entry].id} : std::nullopt;
}

std::optional<std::array<u32, 2>> LegendsDreamcastSounds::motionTokens(char realmLetter, s32 slot,
                                                                       bool boss) const {
    constexpr std::array<std::string_view, 6> families{"MET", "ROPE",  "CHAIN",
                                                       "ICE", "STONE", "ROCK"};
    if (slot < 0 || slot >= static_cast<s32>(families.size()) || realmLetter < 'A' ||
        realmLetter > 'Z')
        return {};
    std::optional<std::array<u32, 2>> fallback;
    for (usize i = 0; i < families.size(); ++i) {
        const auto start = namedToken(
            i == 5 ? "S_ROCKROTATE"
                   : std::format("S_ELV{}{}{}", families[i], realmLetter, boss ? "B" : ""));
        const auto stop = namedToken(
            i == 5 ? "S_ROCKSTOP"
                   : std::format("S_ELV{}STP{}{}", families[i], realmLetter, boss ? "B" : ""));
        if (!start || !stop)
            continue;
        const std::array pair{*start, *stop};
        if (static_cast<s32>(i) == slot)
            return pair;
        if (!fallback)
            fallback = pair;
    }
    return fallback;
}

const DreamcastBankSample* LegendsDreamcastSounds::sample(u32 token) {
    const u32 bank = token >> 16U, index = token & 0xffffU;
    if (bank >= m_directoryTable.banks.size() || index >= m_directoryTable.banks[bank].soundCount)
        return nullptr;
    auto found = m_banks.find(bank);
    if (found == m_banks.end()) {
        // A failed optional bank is remembered, so one missing file does not
        // turn every movement tick into another disk read and warning.
        found = m_banks.emplace(bank, std::vector<DreamcastBankSample>{}).first;
        try {
            const auto& row = m_directoryTable.banks[bank];
            if (row.file.empty() || !std::ranges::all_of(row.file, [](unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_';
                }))
                throw FormatError("Invalid Dreamcast sound bank name");
            const auto path = AssetLocator{m_directory}.find(row.file + ".bnk");
            if (!path)
                return nullptr;
            auto samples = decodeDreamcastSoundBank(readFile(*path));
            if (samples.size() != row.soundCount)
                throw FormatError("Dreamcast sound bank count disagrees with AUD_DATA");
            found->second = std::move(samples);
        } catch (const std::exception& error) {
            log::warn("Dreamcast sound bank {}: {}", bank, error.what());
        }
    }
    if (index >= found->second.size())
        return nullptr;
    return &found->second[index];
}

SoundHandle LegendsDreamcastSounds::play(SoundPlayer& output, u32 token, f32 volume, bool loop) {
    if (!std::isfinite(volume) || volume <= 0)
        return kNoSound;
    const auto* decoded = sample(token);
    if (!decoded)
        return kNoSound;
    const u32 bank = token >> 16U, index = token & 0xffffU;
    SoundSequence sequence;
    sequence.bank = "dreamcast/" + m_directoryTable.banks[bank].file;
    sequence.call = index;
    sequence.steps.push_back({&decoded->clip, true, loop || decoded->loop});
    return output.play(sequence, std::min(volume, 1.0f), SoundCategory::Effects);
}

std::optional<SoundSequence> LegendsDreamcastSounds::phrase(std::span<const u32> tokens) {
    if (tokens.empty() || tokens.size() > 16)
        return {};
    SoundSequence sequence;
    for (const u32 token : tokens) {
        const auto* decoded = sample(token);
        if (!decoded || decoded->loop)
            return {};
        sequence.steps.push_back({&decoded->clip, true, false});
    }
    sequence.bank = "dreamcast/" + m_directoryTable.banks[tokens.front() >> 16U].file;
    sequence.call = tokens.front() & 0xffffU;
    return sequence;
}

u32 dreamcastContainerSound(s32 realm, s32 subtype) {
    if (subtype >= 46 && subtype <= 48)
        return 38; // AudioChest 8c0e0c80, S_CHEST.
    constexpr std::array<std::array<u32, 3>, 9> kSounds{{{},
                                                         {0x230007, 0x230009, 0x230008},
                                                         {0x210024, 0x210026, 0x210025},
                                                         {0x240022, 0x240024, 0x240023},
                                                         {0x250003, 0x250005, 0x250004},
                                                         {},
                                                         {},
                                                         {0x220019, 0x22001b, 0x22001a},
                                                         {0x260019, 0x26001b, 0x26001a}}};
    return realm >= 0 && static_cast<usize>(realm) < kSounds.size() && subtype >= 43 &&
                   subtype <= 45
               ? kSounds[static_cast<usize>(realm)][static_cast<usize>(subtype - 43)]
               : 0;
}

u32 dreamcastGateSound(s32 realm, s32 subtype) {
    constexpr std::array<std::array<u32, 4>, 9> kSounds{{{},
                                                         {0x230006, 0x230004, 0x230005, 0x230003},
                                                         {0x210023, 0x210023, 0x210023, 0x210023},
                                                         {0x24001f, 0x240020, 0x240021, 0x24001f},
                                                         {0x25002b, 0x25002b, 0x25002b, 0x25002b},
                                                         {},
                                                         {},
                                                         {0x220018, 0x220018, 0x220018, 0x220031},
                                                         {0x260018, 0x260018, 0x260018, 0x260018}}};
    return realm >= 0 && static_cast<usize>(realm) < kSounds.size() && subtype >= 0 && subtype < 4
               ? kSounds[static_cast<usize>(realm)][static_cast<usize>(subtype)]
               : 0;
}

std::optional<u32> dreamcastGeneratorSound(s32 realm, bool destroyed) {
    // HitItem 8c0afda2/8c0afe00 calls these independently of its visual state
    // change. Do not use GDL's map-name suffix ("dreamcast:levelb1" starts with d).
    constexpr std::array<std::array<u32, 2>, 8> kSounds{{
        {0x230031, 0x230032},
        {0x21002f, 0x210030},
        {0x24003d, 0x24003e},
        {0x250037, 0x250038},
        {0x2b0018, 0x2b0019},
        {0x2d001b, 0x2d001c},
        {0x220016, 0x220017},
        {0x260016, 0x260017},
    }};
    if (realm < 1 || realm > static_cast<s32>(kSounds.size()))
        return {};
    return kSounds[static_cast<usize>(realm - 1)][destroyed ? 1 : 0];
}

u32 dreamcastCollapseSound(s32 realm, bool boss) {
    // Native realm table 8c1279e4. Its -1 entries mean no sound.
    constexpr std::array<u32, 9> kSounds{
        0, 0x23003b, 0x210001, 0x240029, 0x250001, 0x2c0000, 0x2d001e, 0x22002a, 0};
    if (realm < 0 || static_cast<usize>(realm) >= kSounds.size())
        return 0;
    return realm == 6 && boss ? 0x2e0000 : kSounds[static_cast<usize>(realm)];
}

} // namespace gdl::game
