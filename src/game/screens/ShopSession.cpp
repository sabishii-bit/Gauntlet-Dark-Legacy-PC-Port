#include "game/screens/ShopSession.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "engine/core/Error.h"
namespace gdl::game {
std::array<s32, 5> ShopLane::statsValues(bool previous) const {
    const auto level = previous ? entryLevel : experienceLevel(member.save.experience());
    auto values = entryStats;
    if (!previous) {
        values = member.save.character == kSumnerClass
                     ? masteryStats()
                     : displayStats(stats, level, member.save.progress());
    }
    return {values.strength(), values.armor(), values.magic(), values.speed(),
            std::min(9999, kStartingHealth + 100 * (level - 1))};
}
std::array<s32, 5> ShopLane::statsRevealTicks() const {
    std::array<s32, 5> result{};
    s32 tick = phase == ShopPhase::BeforeStats ? 90 : 30;
    const auto before = statsValues(true);
    const auto after = statsValues(false);
    for (usize i = 0; i < result.size(); ++i) {
        result[i] = tick;
        if (before[i] != after[i]) {
            tick += 60;
        }
    }
    return result;
}
bool ShopLane::statsReady() const {
    const auto ticks = statsRevealTicks();
    const s32 last =
        ticks.back() + (statsValues(true).back() != statsValues(false).back() ? 60 : 0);
    return phaseSeconds * 60 >= last;
}
void ShopLane::rememberShopEntry() {
    entryGold = member.save.gold;
    goldHeight = tally.targetHeight(0);
    entryLevel = experienceLevel(member.save.experience());
    entryStats = member.save.character == kSumnerClass
                     ? masteryStats()
                     : displayStats(stats, entryLevel, member.save.progress());
}
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
        lane.entryLevel =
            experienceLevel(std::max(0, member.save.experience() - lane.tally.results().totals[2]));
        lane.entryStats = member.save.character == kSumnerClass
                              ? masteryStats()
                              : displayStats(lane.stats, lane.entryLevel, member.save.progress());
        if (member.fallen) {
            lane.phase = ShopPhase::Done;
        }
        m_lanes.push_back(std::move(lane));
    }
}
void ShopSession::skipTally() {
    for (auto& lane : m_lanes) {
        if (lane.phase == ShopPhase::Tally) {
            lane.phase = ShopPhase::Shopping;
            lane.rememberShopEntry();
        }
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
        const auto previousPhase = lane.phase;
        lane.phaseSeconds += seconds;
        switch (lane.phase) {
        case ShopPhase::Tally: {
            const bool ready = lane.tally.finished();
            lane.tally.update(seconds);
            // A press on the frame the tally ends cannot also leave it.
            if (ready && input.select) {
                lane.phase = lane.entryLevel == experienceLevel(lane.member.save.experience())
                                 ? ShopPhase::Shopping
                                 : ShopPhase::BeforeStats;
            }
            break;
        }
        case ShopPhase::BeforeStats:
            if (lane.statsReady() && input.select) {
                lane.phase = ShopPhase::Shopping;
            }
            break;
        case ShopPhase::Shopping: {
            const f32 targetHeight = LevelTally::kInitialHeight +
                                     static_cast<f32>(lane.member.save.gold) *
                                         (LevelTally::kMaxHeight - LevelTally::kInitialHeight) /
                                         (static_cast<f32>(lane.entryGold) + 1);
            lane.goldHeight = std::max(
                targetHeight, lane.goldHeight - static_cast<f32>(std::min(seconds, 60.0)) * 60);
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
            if (lane.statsReady() && input.select) {
                lane.phase = ShopPhase::Done;
            }
            break;
        case ShopPhase::Done: break;
        }
        if (lane.phase != previousPhase) {
            lane.phaseSeconds = 0;
            if (lane.phase == ShopPhase::Shopping) {
                lane.rememberShopEntry();
            }
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
