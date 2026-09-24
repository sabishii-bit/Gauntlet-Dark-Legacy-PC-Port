#pragma once

#include <filesystem>
#include <optional>
#include <span>

#include "engine/assets/SoundSet.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "game/players/Party.h"
#include "game/world/PlacedItems.h"

namespace gdl::game {

/** Special pickups actually left in the scene, not merely advertised by its level record. */
struct ExitRelics {
    std::optional<s32> rune = std::nullopt;   ///< zero-based pickup value
    std::optional<s32> legend = std::nullopt; ///< realm of the legendary item
    static ExitRelics remaining(const PlacedItems& items);
};

/** Exit reminders outlive the departing scene. The application owns this bank and its voice;
 * close before destroying SoundPlayer, never as part of ordinary level teardown. */
class LevelExitSpeech {
public:
    LevelExitSpeech() = default;
    LevelExitSpeech(const LevelExitSpeech&) = delete;
    LevelExitSpeech& operator=(const LevelExitSpeech&) = delete;
    LevelExitSpeech(LevelExitSpeech&&) = delete;
    LevelExitSpeech& operator=(LevelExitSpeech&&) = delete;
    ~LevelExitSpeech() = default;

    /** Call once when portal departure begins. Missing/owned relics produce no voice. */
    SoundHandle begin(const std::filesystem::path& root, SoundPlayer* output,
                      const ExitRelics& remaining, std::span<const PartyMember> party);
    void close();

private:
    SoundSet m_bank;
    std::filesystem::path m_root;
    SoundPlayer* m_output = nullptr;
    SoundHandle m_voice = kNoSound;
    usize m_runeCue = 0;
    usize m_legendCue = 0;
};

} // namespace gdl::game
