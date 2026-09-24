#include "game/screens/LevelExitSpeech.h"

#include <algorithm>
#include <array>
#include <exception>
#include <string_view>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr s32 kLegendItem = 13;
constexpr std::array<std::string_view, 4> kRuneCues{"S_SKORNTAUNT1", "S_TOOHASTY", "S_SKORNTAUNT2",
                                                    "S_SRCHBTTR"};
constexpr std::array<std::string_view, 4> kLegendCues{"S_SKORNTAUNT3", "S_QUIK2LEAVE",
                                                      "S_SKORNTAUNT4", "S_FRGTLGND"};
} // namespace

ExitRelics ExitRelics::remaining(const PlacedItems& items) {
    ExitRelics result;
    for (usize i = 0; i < items.size(); ++i) {
        const auto& item = items.item(i);
        if (item.taken || !item.shownTo(items.playerCount())) {
            continue;
        }
        if (item.subtype == ItemInfo::kRunestone) {
            result.rune = item.value;
        } else if (item.subtype == kLegendItem) {
            result.legend = item.value;
        }
    }
    return result;
}

SoundHandle LevelExitSpeech::begin(const std::filesystem::path& root, SoundPlayer* output,
                                   const ExitRelics& remaining,
                                   std::span<const PartyMember> party) {
    if (party.empty()) {
        return kNoSound;
    }
    std::string_view cue;
    // The original checks the legendary item first and returns even when it is owned.
    // These rotations persist between levels; owning it on an earlier visit counts too.
    if (remaining.legend.has_value()) {
        const s32 realm = *remaining.legend;
        if (realm < 0 || realm >= Relics::kRealmCount ||
            std::ranges::any_of(party, [realm](const PartyMember& member) {
                return member.save.progress().relics.hasLegend(realm);
            })) {
            return kNoSound;
        }
        cue = kLegendCues[m_legendCue];
        m_legendCue = (m_legendCue + 1) % kLegendCues.size();
    } else if (remaining.rune.has_value()) {
        const s32 rune = *remaining.rune;
        if (rune < 0 || rune >= Relics::kRuneCount ||
            std::ranges::any_of(party, [rune](const PartyMember& member) {
                return member.save.progress().relics.hasRune(rune);
            })) {
            return kNoSound;
        }
        cue = kRuneCues[m_runeCue];
        m_runeCue = (m_runeCue + 1) % kRuneCues.size();
    } else {
        return kNoSound;
    }
    if (output == nullptr) {
        return kNoSound;
    }
    if (m_output != nullptr) {
        m_output->stop(m_voice);
    }
    m_output = output;
    m_voice = kNoSound;
    if (m_root != root || !m_bank.loaded()) {
        m_root = root;
        if (!m_bank.load(root / "audio/VOICE1")) {
            return kNoSound;
        }
    }
    try {
        if (const auto sound = m_bank.find(cue)) {
            m_voice = output->play(m_bank.sequence(*sound));
            log::info("Level exit reminder: {}", cue);
        } else {
            log::warn("Level exit voice {} is missing from VOICE1", cue);
        }
    } catch (const std::exception& error) {
        log::warn("Level exit voice {}: {}", cue, error.what());
    }
    return m_voice;
}

void LevelExitSpeech::close() {
    if (m_output != nullptr) {
        m_output->stop(m_voice);
    }
    m_voice = kNoSound;
    m_output = nullptr;
    m_bank = {};
    m_root.clear();
}
} // namespace gdl::game
