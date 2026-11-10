#include "expansions/n64/scenes/LegendsSumnerText.h"

namespace gdl::game {
namespace {
bool ceremonyCaption(u32 identity) {
    return identity - kLegendsN64FirstCeremonyCaption < kLegendsN64CeremonyCaptionCount;
}
} // namespace

std::string legendsN64SumnerTextKey(u32 identity) {
    // strlib.wad: advisortower 38..49, pause 425..445, itemedit 446..559.
    // The last bank also owns Sumner's 492..520 topic replies and the ceremony
    // lines after them. e0046000's menu selects 426, 430 and 441..445;
    // e00722d0 advice rows 53..81 select the topic replies, without speech.
    if ((identity >= 38 && identity <= 49) || identity == 426 || identity == 430 ||
        (identity >= 441 && identity <= 445) ||
        (identity >= 492 && identity < kLegendsN64FirstCeremonyCaption) ||
        ceremonyCaption(identity)) {
        return "legends.n64.sumner.text." + std::to_string(identity);
    }
    return {};
}

std::string legendsN64CeremonyCaptionKey(u32 identity) {
    return ceremonyCaption(identity) ? legendsN64SumnerTextKey(identity) : std::string{};
}

std::string legendsN64SumnerHistoryKey(u8 page) {
    // e0071bc8 resolves the Tower's external advice documents. Its level
    // string base is zero, so absent pages have no indexed-text fallback.
    if (page == 1 || page == 2 || (page >= 10 && page <= 17)) {
        return "legends.n64.sumner.history." + std::to_string(page);
    }
    return {};
}

} // namespace gdl::game
