#pragma once

#include <map>
#include <optional>
#include <span>
#include <vector>

#include "expansions/n64/formats/LegendsN64Scene.h"

namespace gdl::game {

/** N64 PPower/PRnd selection is a shared, deterministic cursor, not a random
 * roll at collection. Floor and concealed container slots consume it in pool
 * order. Native e0065c98/e0065e98/e006d7c0 own the timer and lookup rules. */
class LegendsLootSelectors {
public:
    struct Slot {
        formats::LegendsN64Fixture placement;
        bool contained = false;
        bool active = true;
        u16 selector = 0;
        std::optional<usize> selected;
        s32 waiting = 0;
    };
    LegendsLootSelectors() = default;
    LegendsLootSelectors(std::span<const u8> powerups, std::span<const u8> randoms,
                         std::span<const formats::LegendsN64Fixture> fixtures, s32 players);
    std::optional<usize> find(u32 nativeIndex, bool contained) const;
    std::span<const Slot> slots() const { return m_slots; }
    std::optional<formats::LegendsN64Fixture> selected(usize slot) const;
    std::vector<formats::LegendsN64Fixture> outcomes(usize slot) const;
    /** A native query at zero resolves again, without resetting the tick timer. */
    void query(usize slot);
    void freeze(usize slot);
    void disable(usize slot);
    void advance(f32 seconds);
    void tick();

private:
    usize indexOf(u16 key, bool nativeFallback = false) const;
    usize resolve(usize index);
    formats::LegendsN64Fixture choice(const Slot& slot, usize index) const;
    std::vector<formats::LegendsN64Fixture> m_templates;
    std::map<u32, std::vector<u16>> m_groups;
    std::map<u16, u32> m_selectorGroups;
    std::map<u16, s32> m_cursors;
    std::vector<Slot> m_slots;
    f64 m_fraction = 0;
};

} // namespace gdl::game
