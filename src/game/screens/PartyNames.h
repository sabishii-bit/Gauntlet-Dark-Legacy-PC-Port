#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {

/**
 * Each player's name over their head as a level opens (load_player's name_timer, WriteName,
 * player.c 2146): the first six letters of the save's name, an underscore shown as a space,
 * in the `initials` font at half size and white, centred on the body's middle, for 240 ticks
 * that do not run while a message, a scroll or a scripted camera holds play. It borrows the
 * STATIC font sheet; clear it before that is released.
 */
class PartyNames {
public:
    static constexpr s32 kTicks = 240;   ///< 0xF0
    static constexpr usize kLetters = 6; ///< of the save's eight
    static constexpr f32 kScale = 0.5f;
    static constexpr s32 kSpaceWidth = 12; ///< the initials font's (gFontDefs)

    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              TextureSet& staticTextures);
    void clear();

    /** What is written over a character named `saveName`. */
    static std::string shownName(std::string_view saveName);
    /** Where `point` falls on a canvas of `width` by `height` through `clip`, if before it. */
    static std::optional<Vec2> screenOf(const Mat4& clip, const Vec3& point, f32 width, f32 height);

    /** Starts every standing member's name showing again. */
    static void show(std::span<PlayerRuntime> players);
    /** Runs the names down by `ticks`, unless play is `held`. */
    void step(std::span<PlayerRuntime> players, s32 ticks, bool held);
    void draw(Canvas& canvas, std::span<const PlayerRuntime> players, const Mat4& clip, f32 width,
              f32 height) const;

private:
    BitmapFont m_font;
    TextPainter m_text;
    bool m_held = false; ///< names are written only while they count down
};

} // namespace gdl::game
