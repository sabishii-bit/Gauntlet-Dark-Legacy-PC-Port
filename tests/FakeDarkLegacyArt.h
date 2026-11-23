#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/BitmapFont.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/TextPainter.h"

#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
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

/** What `art` lends for `lookup`. */
inline const Texture* lentBy(game::DarkLegacyHudArt& art, const DarkLegacyLookup& lookup) {
    switch (lookup.set) {
    case DarkLegacyLookup::Set::Static: return art.staticArt(lookup.name);
    case DarkLegacyLookup::Set::Select: return art.selectArt(lookup.name);
    case DarkLegacyLookup::Set::Count: return art.countArt(lookup.name);
    case DarkLegacyLookup::Set::Inventory: return art.inventoryArt(lookup.name);
    }
    return nullptr;
}

/** Dark Legacy's art as a test lends it: every lookup is kept, by set and name, and finds the
 * picture the test gave under that name, else none; the small capitals are a synthetic font of
 * the digits and capitals; its own look is a FakeHudPainter under Dark Legacy's id that reports
 * to `record`. */
class FakeDarkLegacyArt final : public game::DarkLegacyHudArt {
public:
    /** The small capitals' characters, side by side on their sheet, each kSmallCapsCell
     * square. */
    static constexpr std::string_view kSmallCapsCodes = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static constexpr s32 kSmallCapsCell = 8;

    FakeDarkLegacyArt() {
        std::vector<BitmapGlyph> glyphs;
        for (usize index = 0; index < kSmallCapsCodes.size(); ++index) {
            glyphs.push_back({.code = kSmallCapsCodes[index],
                              .width = kSmallCapsCell,
                              .x = static_cast<s32>(index) * kSmallCapsCell,
                              .y = 0});
        }
        m_smallCapsFont =
            BitmapFont::fromGlyphs(kSmallCapsCell, kSmallCapsCell / 2, std::move(glyphs));
        m_smallCaps.setFont(&m_smallCapsFont, &m_smallCapsSheet);
    }
    ~FakeDarkLegacyArt() override = default;
    GDL_NON_COPYABLE_NON_MOVABLE(FakeDarkLegacyArt);

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

    /** Lets the lookups of `set`'s picture `name` find a picture `width` by `height`, the same at
     * every ask, kept as long as the art. */
    const Texture& give(DarkLegacyLookup::Set set, std::string_view name, u32 width = 16,
                        u32 height = 16) {
        m_given.push_back({{set, std::string{name}}, std::make_unique<FakeTexture>(width, height)});
        return *m_given.back().picture;
    }
    /** The sheet the small capitals draw with. */
    const Texture& smallCapsSheet() const { return m_smallCapsSheet; }

    std::vector<DarkLegacyLookup> lookups; ///< every lookup, in turn
    FakeHudRecord record;                  ///< what Dark Legacy's own look was asked
    FakeHudPainter darkLegacyLook{std::string{game::kDarkLegacyHudStyle}, record};

private:
    /** A picture the test gave, and the lookup that finds it. */
    struct Given {
        DarkLegacyLookup lookup;
        std::unique_ptr<FakeTexture> picture;
    };

    const Texture* lookUp(DarkLegacyLookup::Set set, std::string_view name) {
        lookups.push_back({set, std::string{name}});
        const auto found = std::ranges::find_if(
            m_given, [&](const Given& given) { return given.lookup == lookups.back(); });
        return found == m_given.end() ? nullptr : found->picture.get();
    }

    std::vector<Given> m_given;
    BitmapFont m_smallCapsFont;
    FakeTexture m_smallCapsSheet{static_cast<u32>(kSmallCapsCodes.size()) * u32{kSmallCapsCell},
                                 u32{kSmallCapsCell}};
    TextPainter m_smallCaps;
};

} // namespace gdl::test
