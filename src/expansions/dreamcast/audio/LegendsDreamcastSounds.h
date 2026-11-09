#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string_view>

#include "engine/audio/SoundPlayer.h"

#include "expansions/dreamcast/audio/DreamcastSoundBank.h"
#include "formats/AudioRom.h"

namespace gdl::game {

/** Native bank/index routing. Optional Dreamcast audio never loads a GameCube
 * replacement or sends its numeric IDs to the unrelated N64 synthesizer. */
class LegendsDreamcastSounds {
public:
    bool open(const std::filesystem::path& nativeRoot);
    bool opened() const { return !m_directoryTable.banks.empty(); }
    std::optional<u32> namedToken(std::string_view name) const;
    /** AudioSetupWorld 8c0dc920: six native movement families, falling back to
     * the first available family when a realm has no dedicated pair. */
    std::optional<std::array<u32, 2>> motionTokens(char realmLetter, s32 slot,
                                                   bool boss = false) const;
    SoundHandle play(SoundPlayer& output, u32 token, f32 volume = 1, bool loop = false);
    /** Finite non-spatial narration. The returned sequence borrows this router's
     * decoded samples, so its owner must outlive every playing/queued handle. */
    std::optional<SoundSequence> phrase(std::span<const u32> tokens);

private:
    const DreamcastBankSample* sample(u32 token);
    std::filesystem::path m_directory;
    formats::AudioRom m_directoryTable;
    std::map<u32, std::vector<DreamcastBankSample>> m_banks;
};

/** AudioGate 8c0e0c40 selects realm/subtype from 8c127bcc. */
u32 dreamcastGateSound(s32 realm, s32 subtype);
/** AudioBarrelWood/Gas/Explosion: realm tables at 8c12793c..8c1279ac. */
u32 dreamcastContainerSound(s32 realm, s32 subtype);
/** AudioGeneratorDamaged/Die: native realm tables at 8c127da8/8c127d84. */
std::optional<u32> dreamcastGeneratorSound(s32 realm, bool destroyed);
/** AudioCollapse 8c0e0100, including the final boss's realm-six override. */
u32 dreamcastCollapseSound(s32 realm, bool boss = false);

} // namespace gdl::game
