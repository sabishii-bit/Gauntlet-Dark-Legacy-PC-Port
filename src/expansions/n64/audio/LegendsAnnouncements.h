#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "engine/audio/SoundClip.h"

#include "game/audio/AnnouncementSubject.h"

namespace gdl::game {
class LegendsSpeech;

/** Preloaded complete narrator equivalents. Resolution never reads assets or
 * decodes audio; the ordinary soundscape retains admission and queue ownership. */
class LegendsAnnouncements {
public:
    bool load(const std::filesystem::path& root, const LegendsSpeech& speech);
    std::optional<SoundSequence> resolve(std::span<const std::string_view> lines,
                                         std::optional<AnnouncementSubject> subject = {}) const;

private:
    struct Cache;
    std::shared_ptr<const Cache> m_cache;
};
} // namespace gdl::game
