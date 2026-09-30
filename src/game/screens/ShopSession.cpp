#include "game/screens/ShopSession.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
namespace gdl::game {
namespace {
constexpr f64 kTickRate = 60.0;
constexpr f64 kLongestUpdate = 60.0;
/** The whole 60 Hz ticks in `seconds`, the rest kept for the next update. */
s32 takeTicks(f64& remainder, f64 seconds) {
    remainder += std::min(seconds, kLongestUpdate) * kTickRate;
    const auto ticks = static_cast<s32>(std::floor(remainder + 1e-9));
    remainder -= ticks;
    return ticks;
}
} // namespace
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
                        ShopCatalog catalog, ShopVisit visit) {
    m_lanes.clear();
    m_events.clear();
    m_catalog = std::move(catalog);
    m_visit = visit;
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
        m_lanes.push_back(std::move(lane));
    }
    // A tower visit has no completed level to tally (do_shop's mode test), and one for the
    // inventory alone opens on the panel.
    for (auto& lane : m_lanes) {
        if (lane.member.fallen) {
            lane.phase = ShopPhase::Done;
        } else if (visit == ShopVisit::Shop) {
            lane.phase = ShopPhase::Shopping;
            lane.rememberShopEntry();
        } else if (visit == ShopVisit::Inventory) {
            enterInventory(lane);
        }
    }
}
void ShopSession::enterInventory(ShopLane& lane) {
    lane.phase = ShopPhase::Inventory;
    lane.inventory.open(InventoryContents::of(lane.member.save.progress()));
    cue(lane, ShopCue::InventoryShown);
}
void ShopSession::cue(const ShopLane& lane, ShopCue cue) {
    m_events.push_back({lane.member.player, cue});
}
std::vector<ShopEvent> ShopSession::takeEvents() {
    return std::exchange(m_events, {});
}
void ShopSession::update(f64 seconds, const Inputs& inputs) {
    if (!std::isfinite(seconds) || seconds < 0) {
        return;
    }
    for (auto& lane : m_lanes) {
        lane.transacted = false;
        const auto& input = inputs[static_cast<usize>(lane.member.player)];
        const auto previousPhase = lane.phase;
        const s32 ticks = takeTicks(lane.tickRemainder, seconds);
        lane.flashTicks = std::max(0, lane.flashTicks - ticks);
        lane.phaseSeconds += seconds;
        switch (lane.phase) {
        case ShopPhase::Tally: {
            const bool ready = lane.tally.finished();
            lane.tally.update(seconds);
            // A press on the frame the tally ends cannot also leave it.
            if (ready && input.select) {
                cue(lane, ShopCue::Select);
                lane.phase = lane.entryLevel == experienceLevel(lane.member.save.experience())
                                 ? ShopPhase::Shopping
                                 : ShopPhase::BeforeStats;
            }
            break;
        }
        case ShopPhase::BeforeStats:
            if (lane.statsReady() && input.select) {
                cue(lane, ShopCue::Select);
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
                cue(lane, ShopCue::CursorPrevious);
            } else if (input.down || input.right) {
                lane.cursor = (lane.cursor + 1) % count;
                cue(lane, ShopCue::CursorNext);
            }
            const auto& item = m_catalog.items()[lane.cursor];
            if (input.select) {
                // Only potion purchases need entropy, and never during deterministic tallying.
                static std::mt19937 random(std::random_device{}());
                const s32 potion =
                    item.type == 3 ? std::uniform_int_distribution<s32>(1, 4)(random) : 1;
                lane.feedback = buyShopItem(lane.member.save, lane.stats, item, potion);
                lane.transacted = true;
                if (lane.feedback == ShopResult::Exit) {
                    lane.phase = ShopPhase::AfterStats;
                } else if (lane.feedback == ShopResult::Bought) {
                    cue(lane, ShopCue::Bought);
                    lane.flashRow = lane.cursor;
                    lane.flashTicks = ShopLane::kFlashTicks;
                } else {
                    cue(lane, ShopCue::Refused);
                }
            } else if (input.back) {
                lane.feedback = sellShopItem(lane.member.save, item);
                lane.transacted = true;
                if (lane.feedback == ShopResult::Sold) {
                    cue(lane, ShopCue::Sold);
                    lane.flashRow = lane.cursor;
                    lane.flashTicks = ShopLane::kFlashTicks;
                } else {
                    cue(lane, ShopCue::Refused);
                }
            } else if (input.start) {
                lane.cursor = 0;
            }
            break;
        }
        case ShopPhase::AfterStats:
            if (lane.statsReady() && input.select) {
                cue(lane, ShopCue::Select);
                if (m_visit == ShopVisit::Level) {
                    enterInventory(lane);
                } else {
                    lane.phase = ShopPhase::Done;
                }
            }
            break;
        case ShopPhase::Inventory:
            if (lane.inventory.step(ticks, input.select)) {
                cue(lane, ShopCue::Select);
            }
            if (lane.inventory.done()) {
                lane.phase = ShopPhase::Done;
            }
            break;
        case ShopPhase::Done: break;
        }
        if (lane.phase != previousPhase) {
            lane.phaseSeconds = 0;
            if (lane.phase == ShopPhase::Shopping) {
                lane.rememberShopEntry();
            } else if (lane.phase == ShopPhase::BeforeStats) {
                cue(lane, ShopCue::LevelGained);
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
