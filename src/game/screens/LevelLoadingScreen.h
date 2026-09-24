#pragma once

#include <string>
#include <vector>

#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"

#include "game/players/Party.h"
#include "game/screens/GameContext.h"

namespace gdl::game {

/** World route, destination highlight and stage preview shown before entering a level.
 * Owns artwork and sound clips; close before the borrowed sound output is destroyed. */
class LevelLoadingScreen {
public:
    static constexpr f32 kMapSeconds = 3.5f;
    static constexpr f32 kPreviewSeconds = 3.0f;
    static constexpr f32 kCrossfadeSeconds = 255.0f / 240.0f;
    void open(const GameContext& context, const LevelRef& level);
    void close();
    bool update(f32 seconds);
    void draw(Canvas& canvas, RenderDevice& device, f32 width);
    bool active() const { return m_active; }
    f32 previewAlpha() const;
    usize dashCount() const;
    const std::string& movie() const { return m_movie; }
    static bool movieWanted(std::string_view movie, std::span<const PartyMember> party);
    static void rememberMovie(std::string_view movie, std::span<PartyMember> party);

private:
    SoundHandle play(std::string_view name, SoundHandle after = kNoSound,
                     SoundCategory category = SoundCategory::Effects);
    void tile(Canvas& canvas, RenderDevice& device, std::string_view name, const Vec2& position,
              f32 alpha, f32 scale);
    TextureSet m_textures;
    SoundSet m_bank;
    SoundPlayer* m_sounds = nullptr;
    std::vector<SoundHandle> m_handles;
    std::vector<Vec2> m_points;
    std::string m_name;
    std::string m_movie;
    f32 m_seconds = 0;
    usize m_dashes = 0;
    bool m_active = false;
};

} // namespace gdl::game
