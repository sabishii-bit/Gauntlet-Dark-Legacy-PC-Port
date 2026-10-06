#include "game/screens/PartyHud.h"

#include <algorithm>
#include <array>
#include <exception>
#include <span>

#include "engine/core/Types.h"

#include "game/menu/OptionMenu.h"
#include "game/menu/ScrollBox.h"
#include "game/players/PickupVoices.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/PartyNames.h"
namespace gdl::game {
namespace {
constexpr std::string_view kScrollTexture = "SCROLL_A";
constexpr std::string_view kHelpTextPrefix = "help";
constexpr std::string_view kStringsFile = "text/english.json";
constexpr std::string_view kSelectorMoveSound = "S_OPTMENUMOVHRZ";
constexpr std::string_view kMenuMoveSound = "S_OPTMENUMOVVRT";
constexpr std::string_view kMenuSelectSound = "S_OPTMENUSEL";

/** AudioAmbientUpdate selects the first standing wearer in native player-ID order. */
const PowerupSlot* stopTimeOf(std::span<const PlayerRuntime> players) {
    const PowerupSlot* selected = nullptr;
    s32 firstPlayer = PartyHud::kPlayerCount;
    for (const auto& player : players) {
        if (player.departed || player.life != PlayerLife::Standing || player.actor.player() < 0 ||
            player.actor.player() >= firstPlayer) {
            continue;
        }
        if (const auto* slot = player.actor.save().progress().inventory.powerup(
                powerup::kSpecial, powerup::kStopTime)) {
            selected = slot;
            firstPlayer = player.actor.player();
        }
    }
    return selected;
}

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
    m_relicTicks = 0;
    m_help.clear();
    m_helpPosition.reset();
    m_pickups.clear();
    m_hourglass.clear();
    m_localStopTimeTotal = 0;
    m_sharedStopTimeTotal = nullptr;
    m_boxes.release();
}

bool PartyHud::bindHourglass(RenderDevice& device, ItemArchive& archive, f32* sharedTotal) {
    m_sharedStopTimeTotal = sharedTotal;
    return m_hourglass.bind(device, archive);
}

void PartyHud::stepHourglass(f32 seconds, std::span<const PlayerRuntime> players) {
    m_hourglass.step(seconds);
    // A scenario or carried save can begin with the item already present. Ordinary pickups
    // record the total in focusPickup; toggling or pausing must not refill the sand.
    if (stopTimeTotal() == 0) {
        if (const auto* slot = stopTimeOf(players)) {
            stopTimeTotal() = slot->strength;
        }
    }
}

bool PartyHud::drawHourglass(Canvas& canvas, std::span<const PlayerRuntime> players) const {
    const auto* slot = stopTimeOf(players);
    if (slot == nullptr) {
        return false;
    }
    // The shared timer uses a signed ratio: two permanent (-1) durations are full,
    // whereas a permanent wearer with another player's finite total is over-empty.
    const f32 ratio = stopTimeTotal() != 0 ? slot->strength / stopTimeTotal() : 0;
    m_hourglass.draw(canvas, ratio, 1, true);
    return true;
}
void PartyHud::drawStatus(Canvas& canvas, std::span<const PlayerRuntime> players) {
    for (s32 player = 0; player < kPlayerCount; ++player) {
        StatusBoxView view = status(player, players, &selector(player));
        view.keysShown = relicsShown();
        m_boxes.draw(canvas, player, view, true);
    }
    m_pickups.draw(canvas, m_boxes);
}
bool PartyHud::postHelp(s32 id, usize index, std::span<PlayerRuntime> players,
                        LevelSoundscape& audio, s32 number, std::optional<Vec3> position) {
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
    const CharacterSave& namedSave = players[index].actor.save();
    const HelpSpeaker speaker{namedSave.character, PickupVoices::carriesPojo(namedSave)};
    const HelpMessageSpec* spec =
        m_help.post(id, position ? -1 : players[index].actor.player(), readers, number, speaker);
    if (spec == nullptr) {
        return false;
    }
    m_helpPosition = position;
    {
        // A turbo attack's name is called from the character's own class's bank; the
        // narrator's lines are in either of its banks. Both wait in the narrator's queue, and
        // an announcement that would wait too long is dropped whole (fn_8009CB44).
        const auto lead = HelpMessages::voiceLead(id, readers.size() > 1);
        const bool named = lead != HelpMessages::VoiceLead::None && !spec->classVoice &&
                           !spec->commonVoice && players[index].figure != nullptr;
        const f32 wait = HelpMessages::voiceWait(id, named ? lead : HelpMessages::VoiceLead::None);
        if (spec->commonVoice) {
            audio.playNamed(spec->voice);
        } else if (!audio.narrationRoom(wait)) {
            return true; // shown, but not heard
        } else if (spec->classVoice && index < players.size() && players[index].figure != nullptr) {
            audio.queueNarrationFrom(players[index].figure->voice(), spec->voice);
        } else if (named) {
            const CharacterSave& save = players[index].actor.save();
            const std::array has{std::string_view{"S_HAS"}, spec->voice};
            const std::span<const std::string_view> lines =
                lead == HelpMessages::VoiceLead::PlayerHas ? std::span{has}
                                                           : std::span{has}.subspan(1);
            audio.announce(players[index].figure->voice(),
                           PickupVoices::nameOf(save.character, save.color),
                           PickupVoices::carriesPojo(save), lines, wait);
        } else {
            audio.queueNarration(spec->voice);
        }
    }
    return true;
}

void PartyHud::stepSelector(PlayerActor& actor, const SelectorInput& input, s32 ticks,
                            LevelSoundscape& audio) {
    const auto slot = static_cast<usize>(std::clamp(actor.player(), 0, kPlayerCount - 1));
    switch (m_selectors[slot].step(input, actor.save().progress().inventory, ticks)) {
    case SelectorCue::Opened:
    case SelectorCue::Closed: audio.playNamed(kMenuMoveSound); break;
    case SelectorCue::Moved: audio.playNamed(kSelectorMoveSound); break;
    case SelectorCue::Switched: audio.playNamed(kMenuSelectSound); break;
    case SelectorCue::None: break;
    }
}

void PartyHud::focusPickup(const PlayerActor& actor, s32 kind, u32 flags) {
    if (actor.player() < 0 || actor.player() >= kPlayerCount) {
        return;
    }
    const Inventory& inventory = actor.save().progress().inventory;
    m_selectors[static_cast<usize>(actor.player())].focus(inventory, kind, flags);
    if (kind == powerup::kSpecial && (flags & powerup::kStopTime) != 0) {
        // PlayerAddPowerup resets one shared sand total to the post-pickup duration, including
        // a duplicate's half-strength extension. This is independent of which wearer is shown.
        for (const auto& slot : inventory.powerups) {
            if (slot.held() && slot.kind == kind && (slot.flags & powerup::kStopTime) != 0) {
                stopTimeTotal() = slot.strength;
                break;
            }
        }
    }
}

StatusBoxView PartyHud::status(s32 player, std::span<const PlayerRuntime> players,
                               const PowerupSelector* selector) {
    StatusBoxView view;
    const auto found = std::ranges::find_if(players, [player](const PlayerRuntime& runtime) {
        return runtime.actor.player() == player;
    });
    if (found == players.end() || found->departed) {
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
        view.towerPrompt = view.inTower && found->towerPrompt;
        view.health = 0;
        return view;
    }
    view.health = save.health();
    view.turbo = found->turbo.look();
    view.keys = save.progress().inventory.keys;
    view.potions = static_cast<s32>(save.progress().inventory.potions.size());
    view.potionKind = save.progress().inventory.nextPotion();
    view.runes = save.progress().relics.runes;
    view.bossKeys = save.progress().relics.shards;
    const Inventory& inventory = save.progress().inventory;
    const s32 usage = selector != nullptr ? selector->usageSlot(inventory)
                                          : PowerupSelector{}.usageSlot(inventory);
    if (usage >= 0) {
        view.powerup = inventory.powerups[static_cast<usize>(usage)];
    }
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
        const s32 chosen = selector.selection();
        if (!selector.showing() || chosen < 0) {
            continue;
        }
        const PowerupSlot& slot =
            actor.save().progress().inventory.powerups[static_cast<usize>(chosen)];
        const std::string_view label = strings->get(powerupTextId(slot.kind, slot.flags));
        const s32 x = actor.player() * StatusBoxPainter::kWidth + PowerupSelector::kLabelX;
        const s32 y = selector.labelY(StatusBoxPainter::kY);
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
                        std::span<const PlayerRuntime> players, const Mat4& clip,
                        const Mat4& canvasProjection, f32 width, f32 height) const {
    if (!m_help.showing()) {
        return;
    }
    Vec2 head{width * 0.5f, height * 0.5f};
    if (m_helpPosition) {
        head = PartyNames::screenOf(clip, *m_helpPosition, canvasProjection).value_or(head);
    }
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() != m_help.player()) {
            continue;
        }
        head = PartyNames::screenOf(clip, actor.attentionPoint(), canvasProjection).value_or(head);
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
