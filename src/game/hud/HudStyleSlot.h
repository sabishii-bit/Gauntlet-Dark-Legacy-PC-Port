#pragma once

#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"

namespace gdl {
class RenderDevice;
}

namespace gdl::game {

/**
 * The look a scene draws its HUD in: the one the settings chose, else Dark Legacy's. A look
 * opens with the scene and changes only when a step settles the slot, after the device has
 * idled, since the frames still in flight may draw with the look let go; one that throws in
 * play is retired for the rest of the scene. Presentation only: never rewound or digested.
 */
class HudStyleSlot {
public:
    /** Lets go of what the slot held and opens look `id` of `styles` for `party`; Dark
     * Legacy's for its own id, an id no offer names, a look that does not open, or no catalog.
     * The device must hold nothing of the looks let go, as at a scene's opening. */
    void open(HudStyles* styles, std::string_view id, RenderDevice& device,
              DarkLegacyHudArt& darkLegacy, std::span<const HudMember> party);
    /** Whether a step must settle the slot: `id` is not the one last asked for, or a look
     * retired in play waits to be let go. */
    bool unsettled(std::string_view id) const;
    /** Lets a retired look go and follows a changed id as open() does. The caller has waited
     * for the device to idle, since a look let go may have drawn the frames still in flight. */
    void settle(std::string_view id, RenderDevice& device, DarkLegacyHudArt& darkLegacy,
                std::span<const HudMember> party);
    /** The look to draw with now. */
    HudPainter& painter(DarkLegacyHudArt& darkLegacy);
    /** Runs one of the look's calls, a frame's drawing or a step's browsing, on painter(). The
     * first exception an expansion's look throws retires it for the rest of the scene with one
     * log line, and that call goes no further; Dark Legacy's look takes the next. What Dark
     * Legacy's own look throws goes on. */
    template <class Call> void run(DarkLegacyHudArt& darkLegacy, Call&& call) {
        if (m_painter == nullptr) {
            std::forward<Call>(call)(darkLegacy.look());
            return;
        }
        try {
            std::forward<Call>(call)(*m_painter);
        } catch (const std::exception& error) {
            retire(error);
        }
    }
    /** The id of the look drawn now. */
    std::string_view shown() const;
    /** Lets every look go, and the catalog. */
    void close();

private:
    /** The painter of the look last asked for; none for Dark Legacy's. */
    std::unique_ptr<HudPainter> openAsked(RenderDevice& device, DarkLegacyHudArt& darkLegacy,
                                          std::span<const HudMember> party);
    void retire(const std::exception& error);

    HudStyles* m_styles = nullptr;
    std::string m_id;                      ///< the id last asked for, opened or not
    std::unique_ptr<HudPainter> m_painter; ///< none: Dark Legacy's
    std::unique_ptr<HudPainter> m_retired; ///< a look that threw, kept until a step lets it go
};

} // namespace gdl::game
