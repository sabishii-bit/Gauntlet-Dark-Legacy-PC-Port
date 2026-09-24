#include "game/screens/ShopSession.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "engine/core/Error.h"
namespace gdl::game {
void ShopSession::start(std::span<const PartyMember> party, std::span<const LevelResults> results,
                        const std::array<s32, 3>& maxima, const ClassDataSet& classes,
                        ShopCatalog catalog) {
    m_lanes.clear();
    m_catalog = std::move(catalog);
    if (party.empty() || party.size() > 4 || m_catalog.items().empty()) {
        throw FormatError("shop: missing party or catalog");
    }
    std::array<bool, 4> joined{};
    for (const auto& member : party) {
        if (member.player < 0 || member.player >= 4 || joined[static_cast<usize>(member.player)]) {
            throw FormatError("shop: invalid or duplicate player");
        }
        joined[static_cast<usize>(member.player)] = true;
        ShopLane lane;
        lane.member = member;
        // Sumner is the hidden Wizard costume, not a seventeenth PDATA record.
        const s32 dataClass = member.save.character == kSumnerClass
                                  ? classIndexOf("WIZ").value_or(-1)
                                  : member.save.character;
        if (const auto* stats = classes.stats(dataClass)) {
            lane.stats = *stats;
        } else {
            throw FormatError("shop: participant's class stats are missing");
        }
        const auto result = std::ranges::find(results, member.player, &LevelResults::player);
        lane.tally.start(result != results.end() ? *result : LevelResults{member.player, {}},
                         maxima);
        if (member.fallen) {
            lane.phase = ShopPhase::Done;
        }
        m_lanes.push_back(std::move(lane));
    }
}
void ShopSession::update(f64 seconds, const Inputs& inputs) {
    if (!std::isfinite(seconds) || seconds < 0) {
        return;
    }
    for (auto& lane : m_lanes) {
        lane.transacted = false;
        lane.feedbackLeft = std::max(0.0, lane.feedbackLeft - seconds);
        const auto& input = inputs[static_cast<usize>(lane.member.player)];
        switch (lane.phase) {
        case ShopPhase::Tally: {
            const bool ready = lane.tally.finished();
            lane.tally.update(seconds);
            // A press on the frame the tally ends cannot also leave it.
            if (ready && input.select) {
                lane.phase = ShopPhase::BeforeStats;
            }
            break;
        }
        case ShopPhase::BeforeStats:
            if (input.select) {
                lane.phase = ShopPhase::Shopping;
            }
            break;
        case ShopPhase::Shopping: {
            const usize count = m_catalog.items().size();
            if (input.up || input.left) {
                lane.cursor = (lane.cursor + count - 1) % count;
            } else if (input.down || input.right) {
                lane.cursor = (lane.cursor + 1) % count;
            }
            const auto& item = m_catalog.items()[lane.cursor];
            if (input.select) {
                // Only potion purchases need entropy, and never during deterministic tallying.
                static std::mt19937 random(std::random_device{}());
                const s32 potion =
                    item.type == 3 ? std::uniform_int_distribution<s32>(1, 4)(random) : 1;
                lane.feedback = buyShopItem(lane.member.save, lane.stats, item, potion);
                lane.feedbackLeft = 1.5;
                lane.transacted = true;
                if (lane.feedback == ShopResult::Exit) {
                    lane.phase = ShopPhase::AfterStats;
                }
            } else if (input.back) {
                lane.feedback = sellShopItem(lane.member.save, item);
                lane.feedbackLeft = 1.5;
                lane.transacted = true;
            } else if (input.start) {
                lane.cursor = 0;
            }
            break;
        }
        case ShopPhase::AfterStats:
            if (input.select) {
                lane.phase = ShopPhase::Done;
            }
            break;
        case ShopPhase::Done: break;
        }
    }
}
bool ShopSession::finished() const {
    return !m_lanes.empty() && std::ranges::all_of(m_lanes, [](const auto& lane) {
        return lane.phase == ShopPhase::Done;
    });
}
std::vector<PartyMember> ShopSession::party() const {
    std::vector<PartyMember> result;
    result.reserve(m_lanes.size());
    for (const auto& lane : m_lanes) {
        result.push_back(lane.member);
    }
    return result;
}
} // namespace gdl::game
