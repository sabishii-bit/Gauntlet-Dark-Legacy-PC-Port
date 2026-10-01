#include <format>

#include "engine/core/Types.h"

#include "game/screens/AfterLevelScene.h"

namespace gdl::game {
void AfterLevelScene::drawFinalStats(const ShopLane& lane, s32 x) {
    // shop_show_final_stats: captions are present immediately, totals appear
    // after 90/150/210/270 ticks, then Continue at 330. The clock is 60 Hz.
    const auto& totals = lane.member.save.progress().lifetime;
    const f64 ticks = lane.phaseSeconds * 60;
    const s32 center = -(x + 64);
    line(center, 8, text("shop.stats"), 0.45f, Color::black());
    line(center, 32, text("shop.finalStats"), 0.56f, Color::white(), true);
    const auto label = [&](s32 y, std::string_view id, s32 start) {
        line(center, y, text(id), 0.48f, Color::white(), ticks > start && ticks < start + 60);
    };
    const auto value = [&](s32 y, s32 total, s32 start) {
        if (ticks > start) {
            line(center, y, std::to_string(total), 0.48f, Color::white(), true);
        }
    };
    label(60, "shop.enemiesKilled", 90);
    value(78, totals.enemiesKilled, 90);
    label(98, "shop.generators", 150);
    label(116, "shop.destroyed", 150);
    value(134, totals.generatorsDestroyed, 150);
    label(154, "shop.goldFound", 210);
    value(172, totals.goldFound, 210);
    label(192, "shop.totalPlaytime", 270);
    if (ticks > 270) {
        const auto minutes = static_cast<s64>(totals.playSeconds / 60);
        line(center, 210, std::format("{} {}", minutes / 1440, text("shop.days")), 0.48f,
             Color::white(), true);
        line(center, 228, std::format("{} {}", minutes / 60 % 24, text("shop.hours")), 0.48f,
             Color::white(), true);
        line(center, 246, std::format("{} {}", minutes % 60, text("shop.minutes")), 0.48f,
             Color::white(), true);
    }
    if (lane.finalStatsReady()) {
        prompt(x + 16, 280, 16);
        line(x + 40, 280, text("shop.continue"), 0.5f, Color::white(), true);
    }
}
} // namespace gdl::game
