#include "expansions/n64/level/LegendsTowerProgress.h"

namespace gdl::game {

LegendsTowerProgress LegendsTowerProgress::of(const LegendsProgress& progress) {
    LegendsTowerProgress result;
    // Tower e0861630/e0862fc4 consume the union of active players' 8210/8260
    // masks (game e004e394/e004e454). Higher saved bits are not native pieces.
    result.runestones = static_cast<u16>(progress.runestones & 0x1fffU);
    result.shards = static_cast<u8>(progress.shards & 0xfU);
    result.cathedralBeam = result.shards == 0xf;
    result.runestonePortal = result.runestones == 0x1fff;
    // game e004e514 -> e006bb1c(realm,0) -> e006b8a4 reads the 24-byte
    // realm table at e006c360. Stage0 has no earlier-stage completion gate.
    // e0054fdc stores positive native obelisk identity N at bit N-1. Bit7
    // is absent from the seven placed obelisks: collecting those seven
    // must NOT inadvertently open Battlefield.
    constexpr std::array<u8, 4> kRequired{0x60, 0x07, 0x18, 0x80};
    for (usize i = 0; i < kRequired.size(); ++i) {
        result.doors[i] = (progress.obelisks & kRequired[i]) == kRequired[i];
    }
    return result;
}

f32 LegendsTowerProgress::shardOpacity() const {
    // e001ea34 maps drawable+1's transparency to 255-transparency.
    return cathedralBeam ? 1.0f : 47.0f / 255.0f;
}

} // namespace gdl::game
