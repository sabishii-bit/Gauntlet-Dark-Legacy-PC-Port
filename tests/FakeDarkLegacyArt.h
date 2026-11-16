#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/TextPainter.h"

#include "FakeHudStyles.h"
#include "game/config/GameConfig.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"

namespace gdl::test {

/** One picture a look asked of Dark Legacy's art: the set it was asked of, and its name. */
struct DarkLegacyLookup {
    enum class Set : u8 { Static, Select, Count, Inventory };
    Set set = Set::Static;
    std::string name;
    bool operator==(const DarkLegacyLookup&) const = default;
};

/** Dark Legacy's art as a test lends it: every lookup is kept, by set and name, and finds no
 * picture; the small capitals have no font; its own look is a FakeHudPainter under Dark
 * Legacy's id that reports to `record`. */
class FakeDarkLegacyArt final : public game::DarkLegacyHudArt {
public:
    const Texture* staticArt(std::string_view name) override {
        return lookUp(DarkLegacyLookup::Set::Static, name);
    }
    const Texture* selectArt(std::string_view name) override {
        return lookUp(DarkLegacyLookup::Set::Select, name);
    }
    const Texture* countArt(std::string_view icon) override {
        return lookUp(DarkLegacyLookup::Set::Count, icon);
    }
    const Texture* inventoryArt(std::string_view name) override {
        return lookUp(DarkLegacyLookup::Set::Inventory, name);
    }
    const TextPainter& smallCaps() const override { return m_smallCaps; }
    game::HudPainter& look() override { return darkLegacyLook; }

    std::vector<DarkLegacyLookup> lookups; ///< every lookup, in turn
    FakeHudRecord record;                  ///< what Dark Legacy's own look was asked
    FakeHudPainter darkLegacyLook{std::string{game::kDarkLegacyHudStyle}, record};

private:
    const Texture* lookUp(DarkLegacyLookup::Set set, std::string_view name) {
        lookups.push_back({set, std::string{name}});
        return nullptr;
    }

    TextPainter m_smallCaps;
};

} // namespace gdl::test
