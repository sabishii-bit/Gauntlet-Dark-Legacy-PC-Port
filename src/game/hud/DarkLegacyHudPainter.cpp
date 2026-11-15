#include "game/hud/DarkLegacyHudPainter.h"

#include <exception>
#include <span>

#include "engine/core/Log.h"
#include "engine/platform/Paths.h"

#include "game/hud/HelpMessages.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/RuneMeter.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PowerupEffects.h"
#include "game/widgets/OptionMenu.h"
#include "game/widgets/ScrollBox.h"

namespace gdl::game {

namespace {

constexpr std::string_view kScrollTexture = "SCROLL_A";
constexpr std::string_view kInventoryDirectory = "INVENTORY";

} // namespace

static_assert(HudView::kSeats == PickupHud::kSlots);

bool DarkLegacyHudPainter::load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                                const StringTable* strings) {
    clear();
    if (!m_boxes.load(device, unpackedRoot, strings)) {
        return false;
    }
    m_device = &device;
    m_root = unpackedRoot;
    return true;
}

void DarkLegacyHudPainter::clear() {
    m_glow = nullptr;
    m_runeFrame = nullptr;
    m_runeColumn = nullptr;
    m_sceneStatic = nullptr;
    m_inventory.releaseTextures();
    m_inventoryTried = false;
    m_root.clear();
    m_device = nullptr;
    m_boxes.release();
}

void DarkLegacyHudPainter::setRuneMeter(const Texture* frame, const Texture* column) {
    m_runeFrame = frame;
    m_runeColumn = column;
}

void DarkLegacyHudPainter::drawStatus(HudFrame& frame, const HudView& view) {
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        m_boxes.draw(frame.canvas, static_cast<s32>(seat), view.seats[seat].box,
                     StatusBoxPainter::Strip::Play);
    }
    std::array<std::optional<s32>, PickupHud::kSlots> labelTops{};
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        const PowerupSelector* selector = view.seats[seat].selector;
        if (selector != nullptr && selector->showing()) {
            labelTops.at(seat) = selector->labelY(StatusBoxPainter::kY);
        }
    }
    if (view.pickups != nullptr) {
        drawPickups(frame.canvas, *view.pickups, labelTops);
    }
}

void DarkLegacyHudPainter::drawPickups(
    Canvas& canvas, const PickupHud& pickups,
    const std::array<std::optional<s32>, PickupHud::kSlots>& labelTops) {
    for (const PickupCard& card : pickups.cards()) {
        m_boxes.drawCard(canvas, card.player, card.texture, card.y);
    }
    for (usize slot = 0; slot < PickupHud::kSlots; ++slot) {
        std::array<StatusBoxPainter::CountEntry, PickupHud::kCountsPerPlayer> row;
        usize shown = 0;
        for (const PickupCount& held : pickups.counts(static_cast<s32>(slot))) {
            if (held.showing()) {
                row.at(shown++) = {.icon = held.icon, .count = held.count, .total = held.total};
            }
        }
        m_boxes.drawCounts(canvas, static_cast<s32>(slot),
                           std::span<const StatusBoxPainter::CountEntry>(row).first(shown),
                           PickupHud::countRowY(labelTops.at(slot)));
    }
}

void DarkLegacyHudPainter::drawRuneMeter(HudFrame& frame, const HudView& view) {
    if (view.runeFill && m_runeFrame != nullptr && m_runeColumn != nullptr) {
        RuneMeter::drawFill(frame.canvas, *m_runeFrame, *m_runeColumn, *view.runeFill);
    }
}

void DarkLegacyHudPainter::drawHourglass(HudFrame& frame, const HudView& view) {
    if (view.hourglass && view.hourglassArt != nullptr) {
        view.hourglassArt->draw(frame.canvas, *view.hourglass);
    }
}

void DarkLegacyHudPainter::drawBossMeters(HudFrame& frame, const HudView& view) {
    if (view.bossMeters != nullptr) {
        view.bossMeters->draw(frame.canvas, frame.device);
    }
}

void DarkLegacyHudPainter::drawSelectors(HudFrame& frame, const HudView& view) {
    if (view.largeText == nullptr || !view.largeText->ready() || view.strings == nullptr) {
        return;
    }
    for (const PlayerRuntime& runtime : view.players) {
        const PlayerActor& actor = runtime.actor;
        const s32 seat = actor.player();
        if (seat < 0 || static_cast<usize>(seat) >= view.seats.size()) {
            continue;
        }
        const PowerupSelector* selector = view.seats[static_cast<usize>(seat)].selector;
        if (selector == nullptr) {
            continue;
        }
        const s32 chosen = selector->selection();
        if (!selector->showing() || chosen < 0) {
            continue;
        }
        const PowerupSlot& slot =
            actor.save().progress().inventory.powerups[static_cast<usize>(chosen)];
        drawSelectorLabel(frame.canvas, *view.largeText, view.strings, m_glow, seat, slot,
                          selector->labelY(StatusBoxPainter::kY));
    }
}

void DarkLegacyHudPainter::drawSelectorLabel(Canvas& canvas, const TextPainter& text,
                                             const StringTable* strings, const Texture* glow,
                                             s32 seat, const PowerupSlot& slot, s32 y) {
    if (!text.ready() || strings == nullptr || seat < 0 ||
        static_cast<usize>(seat) >= HudView::kSeats) {
        return;
    }
    const std::string_view label = strings->get(powerupTextId(slot.kind, slot.flags));
    // A negative x centers the line on the middle of this seat's box. A line that would pass
    // the box shrinks, and keeps the same bottom, just above the runestones.
    const f32 scale = PowerupSelector::labelScale(text, label);
    const s32 x = -((seat * StatusBoxPainter::kWidth) + (StatusBoxPainter::kWidth / 2));
    const s32 fitted = y + text.lineHeight(PowerupSelector::kLabelScale) - text.lineHeight(scale);
    TextStyle style;
    style.scale = scale;
    if (slot.on && glow != nullptr) {
        TextStyle glowing = style;
        glowing.texture = glow;
        glowing.color = ScrollBox::kGlowColor;
        glowing.expand = OptionMenu::kGlowExpand;
        text.draw(canvas, x, fitted, label, glowing);
    }
    text.draw(canvas, x, fitted, label, style);
}

void DarkLegacyHudPainter::drawHelp(HudFrame& frame, const HudView& view) {
    if (!view.help) {
        return;
    }
    const Texture* scroll = nullptr;
    if (m_sceneStatic != nullptr && m_sceneStatic->loaded()) {
        if (const auto sheet = m_sceneStatic->find(kScrollTexture)) {
            try {
                scroll = &m_sceneStatic->texture(frame.device, *sheet);
            } catch (const std::exception&) {
                scroll = nullptr;
            }
        }
    }
    // Written in the strings' own font, the boxes' small capitals.
    HelpMessages::drawLines(frame.canvas, smallCaps(), scroll, view.help->head, view.help->lines,
                            view.help->player);
}

const Texture* DarkLegacyHudPainter::inventoryArt(std::string_view name) {
    if (m_device == nullptr) {
        return nullptr;
    }
    if (!m_inventoryTried) {
        m_inventoryTried = true;
        if (!m_inventory.load(m_root / kInventoryDirectory)) {
            log::warn("HUD: INVENTORY's pictures not found under {}", paths::utf8(m_root));
        }
    }
    const auto index = m_inventory.loaded() ? m_inventory.find(name) : std::nullopt;
    if (!index) {
        return nullptr;
    }
    try {
        return &m_inventory.texture(*m_device, *index);
    } catch (const std::exception& error) {
        log::warn("HUD: INVENTORY picture {}: {}", name, error.what());
        return nullptr;
    }
}

} // namespace gdl::game
