#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/characters/Inventory.h"
#include "game/config/GameConfig.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudView.h"
#include "game/hud/PickupHud.h"
#include "game/hud/StatusBox.h"

namespace gdl::game {

/**
 * Dark Legacy's own look of the play HUD, drawn as the original draws it: the status boxes with
 * the pickup cards and counts over them, the runestone finder, the hourglasses, the boss meters,
 * the selector labels and the help scroll. It owns the boxes' art, and lends that art and its
 * whole look to the other looks. The glow, the finder's pictures, the help scroll's set and a
 * level's pickup icons are borrowed until clear(), which also lets its own art go before the
 * device does.
 */
class DarkLegacyHudPainter final : public HudPainter, public DarkLegacyHudArt {
public:
    DarkLegacyHudPainter() = default;
    ~DarkLegacyHudPainter() override = default;
    GDL_NON_COPYABLE_NON_MOVABLE(DarkLegacyHudPainter);

    /** Loads the boxes' panels and fonts from the unpacked data; false when they are missing. */
    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const StringTable* strings);
    void clear();
    /** FONT32's glow, borrowed from the scene's STATIC set until clear(). */
    void setGlow(const Texture* glow) { m_glow = glow; }
    /** The runestone finder's frame and column, borrowed from the scene's STATIC set until
     * clear(). */
    void setRuneMeter(const Texture* frame, const Texture* column);
    /** A level's own pickup icons, which the tickers prefer; borrowed until clear(). */
    void setCountTextures(TextureSet* textures) { m_boxes.setCountTextures(textures); }
    /** The scene's STATIC set, whose SCROLL_A the help scroll is looked up in as it is drawn;
     * borrowed until clear(). */
    void setHelpScroll(TextureSet* sceneStatic) { m_sceneStatic = sceneStatic; }

    std::string_view id() const override { return kDarkLegacyHudStyle; }
    f32 statusTop() const override { return static_cast<f32>(StatusBoxPainter::kBarY); }
    void drawStatus(HudFrame& frame, const HudView& view) override;
    void drawRuneMeter(HudFrame& frame, const HudView& view) override;
    void drawHourglass(HudFrame& frame, const HudView& view) override;
    void drawBossMeters(HudFrame& frame, const HudView& view) override;
    void drawSelectors(HudFrame& frame, const HudView& view) override;
    void drawHelp(HudFrame& frame, const HudView& view) override;

    const Texture* staticArt(std::string_view name) override { return m_boxes.staticTexture(name); }
    const Texture* selectArt(std::string_view name) override { return m_boxes.selectTexture(name); }
    const Texture* countArt(std::string_view icon) override { return m_boxes.countTexture(icon); }
    /** Reads INVENTORY on the first ask; a set that could not be read gives null from then on. */
    const Texture* inventoryArt(std::string_view name) override;
    const TextPainter& smallCaps() const override { return m_boxes.smallCaps(); }
    HudPainter& look() override { return *this; }

    /** The pickup cards over the boxes, then each seat's counts, kept clear of its label:
     * `labelTops` holds the top of each seat's selector label while one is up. */
    void drawPickups(Canvas& canvas, const PickupHud& pickups,
                     const std::array<std::optional<s32>, PickupHud::kSlots>& labelTops);
    /** One selector label: the original's FONT32 at 0.45 over seat `seat`'s box, glowing while
     * the powerup is worn. */
    static void drawSelectorLabel(Canvas& canvas, const TextPainter& text,
                                  const StringTable* strings, const Texture* glow, s32 seat,
                                  const PowerupSlot& slot, s32 y);

private:
    StatusBoxPainter m_boxes;
    TextureSet m_inventory;
    bool m_inventoryTried = false;
    std::filesystem::path m_root;
    RenderDevice* m_device = nullptr;
    TextureSet* m_sceneStatic = nullptr;
    const Texture* m_glow = nullptr;
    const Texture* m_runeFrame = nullptr;
    const Texture* m_runeColumn = nullptr;
};

} // namespace gdl::game
