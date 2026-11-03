#pragma once
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/hud/PartyHud.h"
#include "game/party/PickupPresentation.h"
#include "game/players/PlayerActor.h"

namespace gdl::test {
/** What the pickups put up, written down in turn ("card 3 CRYSTAL", "count 3 SM_FANGS 12/12",
 * "focus 3 6 0", "relics") and passed on to a party HUD when it is given one. */
class RecordingPickupPresentation final : public game::PickupPresentation {
public:
    RecordingPickupPresentation() = default;
    explicit RecordingPickupPresentation(game::PartyHud& hud) : m_hud(&hud) {}

    void addCard(s32 seat, std::string_view texture) override {
        log.push_back(std::format("card {} {}", seat, texture));
        if (m_hud != nullptr) {
            m_hud->pickups().addCard(seat, texture);
        }
    }
    void showCount(s32 seat, std::string_view icon, s32 count, s32 total) override {
        log.push_back(std::format("count {} {} {}/{}", seat, icon, count, total));
        if (m_hud != nullptr) {
            m_hud->pickups().showCount(seat, icon, count, total);
        }
    }
    void focusPickup(const game::PlayerActor& actor, s32 kind, u32 flags) override {
        log.push_back(std::format("focus {} {} {}", actor.player(), kind, flags));
        if (m_hud != nullptr) {
            m_hud->focusPickup(actor, kind, flags);
        }
    }
    void showRelics() override {
        log.emplace_back("relics");
        if (m_hud != nullptr) {
            m_hud->showRelics();
        }
    }

    std::vector<std::string> log; ///< every call in turn

private:
    game::PartyHud* m_hud = nullptr;
};
} // namespace gdl::test
