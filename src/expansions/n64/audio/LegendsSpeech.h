#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/SoundPlayer.h"

#include "expansions/n64/audio/MortBank.h"
#include "game/audio/AnnouncementSubject.h"

namespace gdl::game {

struct LegendsSpeechCue {
    std::vector<u16> clips;
    f32 lifetimeSeconds = 1;
    bool clearQueue = false;
};

struct LegendsDeathAdvice {
    s32 maximumLevel = 0;
    u32 speech = 0;
    std::string text;
};

/** Optional N64 Sumner narration. Contextual phrases require an explicit
 * supported player identity; rank substitutions remain outside gameplay cues.
 * The scene owns the SoundPlayer lifetime and calls stop before unloading.
 * Effects-category pause freezes the audible cursor; update(paused=true)
 * additionally freezes pending admission and expiry without changing other audio. */
class LegendsSpeech {
public:
    bool openN64(const std::filesystem::path& nativeRoot);
    bool opened() const { return !m_entries.empty() && m_bank.opened(); }
    std::optional<LegendsSpeechCue> resolve(u32 token) const;
    std::optional<LegendsSpeechCue> resolve(u32 token, const AnnouncementSubject& subject) const;
    std::optional<u32> entryCue(std::string_view mapName) const;
    std::optional<u32> obeliskCue(u32 nativeIdentity) const;
    const std::optional<LegendsDeathAdvice>& deathAdvice() const { return m_deathAdvice; }

    bool enqueue(SoundPlayer& output, u32 token, s32 priority = 5);
    void update(SoundPlayer& output, f32 seconds, bool paused = false);
    void stop(SoundPlayer& output);
    SoundHandle handle() const { return m_handle; }
    usize pendingCount() const { return m_pending.size(); }
    bool playing(const SoundPlayer& output) const { return output.isPlaying(m_handle); }

private:
    std::optional<LegendsSpeechCue> resolve(u32 token, const AnnouncementSubject* subject) const;
    struct Entry {
        u16 token = 0xffff;
        u16 replacement = 0xffff;
        u8 flags = 0;
        f32 lifetime = 1;
    };
    struct Pending {
        LegendsSpeechCue cue;
        s32 priority = 5;
        f32 age = 0;
    };
    MortBank m_bank;
    std::vector<Entry> m_entries;
    Entry m_default;
    std::vector<std::vector<u16>> m_groups;
    std::array<u16, 4> m_colors{};
    std::array<u16, 10> m_classes{};
    std::map<std::string, u32, std::less<>> m_levelCues;
    std::array<u32, 3> m_obeliskCues{};
    std::optional<LegendsDeathAdvice> m_deathAdvice;
    std::vector<Pending> m_pending;
    SoundHandle m_handle = kNoSound;
    bool m_paused = false;
};

} // namespace gdl::game
