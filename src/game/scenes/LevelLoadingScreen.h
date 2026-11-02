#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"

#include "game/characters/Party.h"
#include "game/context/GameContext.h"
#include "game/hud/StatusBox.h"
#include "game/scenes/TransitionScreen.h"

namespace gdl::game {

/** World route, destination highlight and stage preview shown before entering a level.
 * Owns artwork and sound clips; close before the borrowed sound output is destroyed. */
class LevelLoadingScreen {
public:
    static constexpr f32 kMapSeconds = 3.5f;
    static constexpr f32 kPreviewSeconds = 3.0f;
    static constexpr f32 kCrossfadeSeconds = 255.0f / 240.0f;
    static constexpr f32 kTransitionSeconds = 2.0f;
    /** Secret visits use the static picture; transitionOnly also selects it on return. */
    void open(RenderDevice& device, const GameContext& context, const LevelRef& level,
              std::span<const PartyMember> party = {}, bool transitionOnly = false);
    void close();
    bool update(f32 seconds);
    /** Draws map and status panels together in the retail 512x384 canvas. */
    void draw(Canvas& canvas, RenderDevice& device);
    bool active() const { return m_active; }
    f32 previewAlpha() const;
    usize dashCount() const;
    const std::string& movie() const { return m_movie; }
    static bool movieWanted(std::string_view movie, std::span<const PartyMember> party);

private:
    /** The four tiles named `prefix`_00..03 joined into one picture at their places and
     * mended across their joins (MapPicture), so no seam shows however the picture is
     * scaled; empty with a tile missing. */
    struct Picture {
        std::unique_ptr<Texture> texture;
        f32 width = 0;
        f32 height = 0;
    };
    SoundHandle play(std::string_view name, SoundHandle after = kNoSound);
    void tile(Canvas& canvas, RenderDevice& device, std::string_view name, const Vec2& position,
              f32 alpha);
    Picture join(RenderDevice& device, std::string_view prefix);
    /** Draws the joined picture, or its tiles one by one when it could not be joined. */
    void picture(Canvas& canvas, RenderDevice& device, const Picture& joined,
                 std::string_view prefix, f32 alpha);
    TextureSet m_textures;
    Picture m_map;
    Picture m_preview;
    TransitionScreen m_transition;
    StatusBoxPainter m_boxes;
    std::array<StatusBoxView, 4> m_status;
    SoundSet m_bank;
    SoundPlayer* m_sounds = nullptr;
    std::vector<SoundHandle> m_handles;
    std::vector<Vec2> m_points;
    std::string m_name;
    std::string m_movie;
    f32 m_seconds = 0;
    usize m_dashes = 0;
    bool m_active = false;
    bool m_transitionOnly = false;
};

} // namespace gdl::game
