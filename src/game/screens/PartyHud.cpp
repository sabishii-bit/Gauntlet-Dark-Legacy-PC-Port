#include "game/screens/PartyHud.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>

#include "game/menu/OptionMenu.h"
#include "game/menu/ScrollBox.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
namespace gdl::game {
namespace {
constexpr std::string_view kScrollTexture = "SCROLL_A";
constexpr std::string_view kHelpTextPrefix = "help";
constexpr std::string_view kStringsFile = "text/english.json";
constexpr std::string_view kSelectorMoveSound = "S_OPTMENUMOVHRZ";
constexpr std::string_view kMenuMoveSound = "S_OPTMENUMOVVRT";
constexpr std::string_view kMenuSelectSound = "S_OPTMENUSEL";

} // namespace
bool PartyHud::load(RenderDevice& device, const std::filesystem::path& root,
                    const StringTable* strings) {
    clear();
    if (!m_boxes.load(device, root, strings)) {
        return false;
    }
    m_strings.load(root / kStringsFile);
    if (strings != nullptr) {
        m_strings.translate(*strings, kHelpTextPrefix);
    }
    m_help.setTexts(&m_strings);
    return true;
}
void PartyHud::clear() {
    for (PowerupSelector& selector : m_selectors) {
        selector.close();
    }
    m_glowSheet = nullptr;
    m_help.clear();
    m_pickups.clear();
    m_boxes.release();
}
void PartyHud::drawStatus(Canvas& canvas, std::span<const PlayerRuntime> players) {
    for (std::int32_t player = 0; player < kPlayerCount; ++player) {
        m_boxes.draw(canvas, player, status(player, players), true);
    }
    m_pickups.draw(canvas, m_boxes);
}
bool PartyHud::postHelp(std::int32_t id, std::size_t index, std::span<PlayerRuntime> players,
                        LevelSoundscape& audio, std::int32_t number) {
    if (index >= players.size()) {
        return false;
    }
    std::vector<HelpReader> readers;
    for (PlayerRuntime& player : players) {
        if (player.life == PlayerLife::Standing) {
            readers.push_back(HelpReader{player.actor.player(), &player.actor.save().helpSeen,
                                         &player.helpHeard});
        }
    }
    const HelpMessageSpec* spec = m_help.post(id, players[index].actor.player(), readers, number);
    if (spec == nullptr) {
        return false;
    }
    {
        // A turbo attack's name is called from the character's own class's bank; the
        // narrator's lines are in either of its banks.
        if (spec->classVoice && index < players.size() && players[index].figure != nullptr) {
            audio.playFrom(players[index].figure->voice(), spec->voice);
        } else {
            audio.narrate(spec->voice);
        }
    }
    return true;
}

void PartyHud::stepSelector(PlayerActor& actor, const SelectorInput& input, std::int32_t ticks,
                            LevelSoundscape& audio) {
    const auto slot = static_cast<std::size_t>(std::clamp(actor.player(), 0, kPlayerCount - 1));
    switch (m_selectors[slot].step(input, actor.save().progress().inventory, ticks)) {
    case SelectorCue::Opened:
    case SelectorCue::Closed: audio.playNamed(kMenuMoveSound); break;
    case SelectorCue::Moved: audio.playNamed(kSelectorMoveSound); break;
    case SelectorCue::Switched: audio.playNamed(kMenuSelectSound); break;
    case SelectorCue::None: break;
    }
}

StatusBoxView PartyHud::status(std::int32_t player, std::span<const PlayerRuntime> players) {
    StatusBoxView view;
    const auto found = std::ranges::find_if(players, [player](const PlayerRuntime& runtime) {
        return runtime.actor.player() == player;
    });
    if (found == players.end()) {
        return view;
    }
    const PlayerActor* actor = &found->actor;
    const CharacterSave& save = actor->save();
    view.mode = StatusBoxView::Mode::Status;
    view.active = true;
    view.classIndex = save.character;
    view.color = save.color;
    view.name = save.name;
    view.level = experienceLevel(save.experience());
    view.gold = save.gold;
    if (found->life != PlayerLife::Standing) {
        view.inTower = found->life == PlayerLife::InTower;
        view.health = 0;
        return view;
    }
    view.health = save.health();
    view.turbo = found->turbo.look();
    view.keys = save.progress().inventory.keys;
    view.potions = static_cast<std::int32_t>(save.progress().inventory.potions.size());
    view.potionKind = save.progress().inventory.nextPotion();
    return view;
}

void PartyHud::drawSelectors(Canvas& canvas, const TextPainter& text, const StringTable* strings,
                             std::span<const PlayerRuntime> players) const {
    if (!text.ready() || strings == nullptr) {
        return;
    }
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        const PowerupSelector& selector = this->selector(actor.player());
        const std::int32_t chosen = selector.selection();
        if (!selector.showing() || chosen < 0) {
            continue;
        }
        const PowerupSlot& slot =
            actor.save().progress().inventory.powerups[static_cast<std::size_t>(chosen)];
        const std::string_view label = strings->get(powerupTextId(slot.kind, slot.flags));
        const std::int32_t x = actor.player() * StatusBoxPainter::kWidth + PowerupSelector::kLabelX;
        const std::int32_t y = selector.labelY(StatusBoxPainter::kY);
        TextStyle style;
        style.scale = PowerupSelector::kLabelScale;
        if (slot.on && m_glowSheet != nullptr) {
            TextStyle glow = style;
            glow.texture = m_glowSheet;
            glow.color = ScrollBox::kGlowColor;
            glow.expand = OptionMenu::kGlowExpand;
            text.draw(canvas, x, y, label, glow);
        }
        text.draw(canvas, x, y, label, style);
    }
}

void PartyHud::drawHelp(Canvas& canvas, RenderDevice& device, TextureSet& textures,
                        std::span<const PlayerRuntime> players, const Mat4& clip, float width,
                        float height) const {
    if (!m_help.showing()) {
        return;
    }
    Vec2 head{width * 0.5f, height * 0.5f};
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() != m_help.player()) {
            continue;
        }
        const Vec4 point = clip * Vec4{actor.followPoint(), 1.0f};
        if (point.w > 1e-4f) {
            head = Vec2{(point.x / point.w * 0.5f + 0.5f) * width,
                        (0.5f - point.y / point.w * 0.5f) * height};
        }
    }
    const auto sheet = textures.loaded() ? textures.find(kScrollTexture) : std::nullopt;
    const Texture* scroll = nullptr;
    if (sheet.has_value()) {
        try {
            scroll = &textures.texture(device, *sheet);
        } catch (const std::exception&) {
            scroll = nullptr;
        }
    }
    m_help.draw(canvas, m_boxes.smallCaps(), scroll, head); // the strings' own font
}
} // namespace gdl::game
