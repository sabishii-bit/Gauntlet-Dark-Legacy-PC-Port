#pragma once
#include <array>
#include <span>
#include <vector>

#include "engine/assets/ShopCatalog.h"
#include "engine/core/Types.h"

#include "game/menu/MenuInput.h"
#include "game/players/LevelResults.h"
#include "game/players/Party.h"
#include "game/players/ShopPurchase.h"
namespace gdl::game {
enum class ShopPhase : u8 { Tally, BeforeStats, Shopping, AfterStats, Done };
struct ShopLane {
    PartyMember member;
    ClassStats stats;
    LevelTally tally;
    ShopPhase phase = ShopPhase::Tally;
    usize cursor = 0;
    ShopResult feedback = ShopResult::Invalid;
    f64 feedbackLeft = 0;
    bool transacted = false; ///< a one-update event, including refused attempts
};
/** The end-level flow independent of rendering/audio. Input addresses player IDs, not
 * vector positions. Fallen members keep their rollback saves but cannot shop. */
class ShopSession {
public:
    using Inputs = std::array<MenuInput, 4>;
    void start(std::span<const PartyMember> party, std::span<const LevelResults> results,
               const std::array<s32, 3>& maxima, const ClassDataSet& classes, ShopCatalog catalog);
    void update(f64 seconds, const Inputs& inputs);
    bool finished() const;
    const std::vector<ShopLane>& lanes() const { return m_lanes; }
    const ShopCatalog& catalog() const { return m_catalog; }
    std::vector<PartyMember> party() const;

private:
    ShopCatalog m_catalog;
    std::vector<ShopLane> m_lanes;
};
} // namespace gdl::game
