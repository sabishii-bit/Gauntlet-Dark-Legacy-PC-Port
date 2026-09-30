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
#include "game/screens/InventoryPanel.h"
namespace gdl::game {
enum class ShopPhase : u8 { Tally, BeforeStats, Shopping, AfterStats, Inventory, Done };
/** What the screen is opened for (init_shop's mode): a level's tally, shop and inventory
 * panel; the tower's shop alone; or the tower's inventory panel alone. */
enum class ShopVisit : u8 { Level, Shop, Inventory };
/** What the flow asks to be heard, in the original's order of events. */
enum class ShopCue : u8 {
    Select,         ///< a page confirmed (AudioCursorSelect)
    CursorNext,     ///< the shop cursor moved on to the next row (AudioClick 0)
    CursorPrevious, ///< and back (AudioClick 1)
    Bought,         ///< a purchase made (fn_8009D038); leaving by Exit is silent
    Sold,           ///< a sale made
    Refused,        ///< a purchase or sale that could not be (AudioBuzzer)
    LevelGained,    ///< the level panel opened on a level gained (AudioExp)
    InventoryShown, ///< the inventory panel began to fly in (AudioTowerFX 2)
};
struct ShopEvent {
    s32 player = 0;
    ShopCue cue = ShopCue::Select;
    bool operator==(const ShopEvent&) const = default;
};
struct ShopLane {
    /** How long a traded row's price shows red. */
    static constexpr s32 kFlashTicks = 30;
    PartyMember member;
    ClassStats stats;
    LevelTally tally;
    ShopPhase phase = ShopPhase::Tally;
    usize cursor = 0;
    ShopResult feedback = ShopResult::Invalid;
    bool transacted = false; ///< a one-update event, including refused attempts
    usize flashRow = 0;      ///< the row last traded, its price red while `flashTicks` last
    s32 flashTicks = 0;
    s32 entryLevel = 1;
    StatBlock entryStats;
    f64 phaseSeconds = 0;
    s32 entryGold = 0;
    f32 goldHeight = LevelTally::kInitialHeight;
    InventoryPanel inventory;
    f64 tickRemainder = 0; ///< the 60 Hz ticks not yet taken from the seconds given
    /** Strength, armor, magic, speed, max health, in the shop's row order. */
    std::array<s32, 5> statsValues(bool previous) const;
    std::array<s32, 5> statsRevealTicks() const;
    bool statsReady() const;
    void rememberShopEntry();
};
/** The end-level flow independent of rendering/audio. Input addresses player IDs, not
 * vector positions. Fallen members keep their rollback saves but cannot shop. */
class ShopSession {
public:
    using Inputs = std::array<MenuInput, 4>;
    void start(std::span<const PartyMember> party, std::span<const LevelResults> results,
               const std::array<s32, 3>& maxima, const ClassDataSet& classes, ShopCatalog catalog,
               ShopVisit visit = ShopVisit::Level);
    void update(f64 seconds, const Inputs& inputs);
    bool finished() const;
    const std::vector<ShopLane>& lanes() const { return m_lanes; }
    const ShopCatalog& catalog() const { return m_catalog; }
    ShopVisit visit() const { return m_visit; }
    std::vector<PartyMember> party() const;
    /** The cues asked for since last taken, oldest first. */
    std::vector<ShopEvent> takeEvents();

private:
    void enterInventory(ShopLane& lane);
    void cue(const ShopLane& lane, ShopCue cue);
    ShopCatalog m_catalog;
    std::vector<ShopLane> m_lanes;
    std::vector<ShopEvent> m_events;
    ShopVisit m_visit = ShopVisit::Level;
};
} // namespace gdl::game
