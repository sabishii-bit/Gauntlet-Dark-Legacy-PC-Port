#include "game/hud/HudStyleSlot.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/Log.h"

#include "game/config/GameConfig.h"

namespace gdl::game {

void HudStyleSlot::open(HudStyles* styles, std::string_view id, RenderDevice& device,
                        DarkLegacyHudArt& darkLegacy, std::span<const HudMember> party) {
    std::string asked{id}; // `id` may name a look about to be let go
    close();
    m_styles = styles;
    m_id = std::move(asked);
    m_painter = openAsked(device, darkLegacy, party);
}

bool HudStyleSlot::unsettled(std::string_view id) const {
    return id != m_id || m_retired != nullptr;
}

void HudStyleSlot::settle(std::string_view id, RenderDevice& device, DarkLegacyHudArt& darkLegacy,
                          std::span<const HudMember> party) {
    std::string asked{id};
    m_retired.reset();
    if (asked != m_id) {
        m_painter.reset(); // its art goes before the next look takes its own
        m_id = std::move(asked);
        m_painter = openAsked(device, darkLegacy, party);
    }
}

HudPainter& HudStyleSlot::painter(DarkLegacyHudArt& darkLegacy) {
    return m_painter != nullptr ? *m_painter : darkLegacy.look();
}

std::string_view HudStyleSlot::shown() const {
    return m_painter != nullptr ? m_painter->id() : kDarkLegacyHudStyle;
}

void HudStyleSlot::close() {
    m_painter.reset();
    m_retired.reset();
    m_styles = nullptr;
    m_id.clear();
}

std::unique_ptr<HudPainter> HudStyleSlot::openAsked(RenderDevice& device,
                                                    DarkLegacyHudArt& darkLegacy,
                                                    std::span<const HudMember> party) {
    if (m_styles == nullptr || m_id == kDarkLegacyHudStyle) {
        return nullptr;
    }
    // An id no expansion offers, such as one a later build knows, is Dark Legacy's without a
    // word; a look offered is asked for even when it cannot be drawn, so that its opener says
    // why.
    const std::span<const HudStyleOffer> offers = m_styles->offers();
    if (std::ranges::none_of(offers,
                             [this](const HudStyleOffer& offer) { return offer.id == m_id; })) {
        return nullptr;
    }
    return m_styles->open(m_id, device, darkLegacy, party);
}

void HudStyleSlot::retire(const std::exception& error) {
    log::warn("HUD: the {} look failed ({}); drawing Dark Legacy's for the rest of the scene", m_id,
              error.what());
    m_retired = std::move(m_painter);
}

} // namespace gdl::game
