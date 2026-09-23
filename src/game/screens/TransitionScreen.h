#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "engine/assets/TextureSet.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"

namespace gdl::game {

/**
 * The picture that covers the play view between levels, as the original's does: it comes up
 * over two seconds once the party has gone through a portal, stays while the next level
 * loads, and clears once the party has arrived. It covers the view above the status boxes,
 * which stay in sight.
 */
class TransitionScreen {
public:
    static constexpr std::string_view kTexture = "TRANSITION_SCREEN";
    static constexpr float kFadeInSeconds = 2.0f;  ///< the original's, leaving a level
    static constexpr float kFadeOutSeconds = 0.5f; ///< arriving; the original's was not found
    static constexpr float kViewHeight = 320.0f;   ///< of the 384 rows: the rest is the boxes'

    enum class Phase : std::uint8_t { Off, ComingUp, Covering, Clearing };

    /** Finds the picture in the unpacked static archive; false (with a warning) without it,
     * in which case the screen covers the view in black. */
    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void release();

    /** Starts the picture coming up; covering() once it hides the view. */
    void comeUp();
    /** Puts the picture up at once, as a level opens under it. */
    void cover();
    /** Starts the picture clearing from wherever it stands. */
    void clearAway();
    void update(float seconds);

    Phase phase() const { return m_phase; }
    bool showing() const { return m_phase != Phase::Off; }
    bool covering() const { return m_phase == Phase::Covering; }
    /** How much of the view the picture hides, 0 to 1. */
    float opacity() const { return m_opacity; }

    /** Draws the picture over a view `width` across, on the canvas's virtual screen. */
    void draw(Canvas& canvas, float width) const;

private:
    TextureSet m_textures;
    const Texture* m_picture = nullptr;
    Phase m_phase = Phase::Off;
    float m_opacity = 0.0f;
};

} // namespace gdl::game
