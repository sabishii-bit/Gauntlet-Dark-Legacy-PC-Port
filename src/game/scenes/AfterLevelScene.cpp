#include "game/scenes/AfterLevelScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <string>
#include <utility>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/audio/ShopMusic.h"
#include "game/config/ControlProfiles.h"
#include "game/players/PickupVoices.h"
#include "game/shop/ShopLayout.h"
#include "game/widgets/MenuFont.h"
#include "game/widgets/OptionMenu.h"
#include "game/widgets/PointerTargets.h"

namespace gdl::game {
namespace {
constexpr std::string_view kHelpTextPrefix = "help";
constexpr std::string_view kSelectSound = "S_OPTMENUSEL";
constexpr std::string_view kCursorNextSound = "S_SECRETCLOCK2";
constexpr std::string_view kCursorPreviousSound = "S_SECRETCLOCK1";
constexpr std::string_view kTradeSound = "S_PICKUPMAGIC";
constexpr std::string_view kRefusedSound = "S_NO";
constexpr std::string_view kInventorySound = "S_STNDGLASS";
constexpr std::string_view kHasSound = "S_HAS";
constexpr std::string_view kGainedLevelSound = "S_GAINEDLEVEL";
constexpr std::string_view kPojoName = "S_POJO2";
constexpr std::string_view kSoundDirectory = "audio";
constexpr s32 kInitialScrollSpeed = 2; ///< pixels per 60 Hz tick

MenuInput shopInput(MenuInput input, const GameConfig& config, s32 player) {
    if (input.devices == nullptr) {
        return input;
    }
    const auto source = playerInputSource(*input.devices, config, player);
    const auto& bindings = menuBindings(config, player);
    const auto pressed = [&](const auto& keys, const auto& buttons) {
        return (source.keyboard && std::ranges::any_of(keys,
                                                       [&](Key key) {
                                                           return key != Key::MouseLeft &&
                                                                  key != Key::MouseRight &&
                                                                  input.devices->wasKeyPressed(key);
                                                       })) ||
               std::ranges::any_of(buttons, [&](PadButton button) {
                   return input.devices->wasPadButtonPressed(source.pad, button);
               });
    };
    // Retail has separate Buy, Sell and Exit buttons. Keep Start as an alias, but
    // shared Select/Start presses (Enter) remain purchases even when scrolling drops them.
    const bool start = input.start && !input.select;
    input.back = pressed(bindings.shopSell, bindings.padShopSell);
    input.start = start || pressed(bindings.shopExit, bindings.padShopExit);
    return input;
}
} // namespace
MenuInput AfterLevelScene::captureShopInput(MenuInput input, const GameConfig& config, s32 player) {
    return shopInput(std::move(input), config, player);
}
bool AfterLevelScene::open(RenderDevice& device, const GameServices& services,
                           std::span<const PartyMember> party,
                           std::span<const LevelResults> results, const std::array<s32, 3>& maxima,
                           std::string_view levelName, ShopVisit visit) {
    close();
    m_services = &services;
    m_device = &device;
    m_time = 0;
    m_frameTime = 0;
    try {
        ShopCatalog catalog;
        ClassDataSet classes;
        if (!catalog.loadRoot(services.unpackedRoot) ||
            !classes.load(services.unpackedRoot / "pdata") ||
            !m_select.load(services.unpackedRoot / "SELECT") ||
            !m_inventory.load(services.unpackedRoot / "INVENTORY") ||
            !m_static.load(services.unpackedRoot / "STATIC") ||
            !loadMenuFont(m_font, services.unpackedRoot, 16) ||
            !m_titles.load(services.unpackedRoot / "text/english.json")) {
            close();
            return false;
        }
        if (services.strings != nullptr) {
            m_titles.translate(*services.strings, kHelpTextPrefix);
            if (services.config != nullptr && services.config->game.expandedShop) {
                expandShopCatalog(catalog, *services.strings);
            }
        }
        const auto* font = texture("FONT32");
        m_glow = texture("FONT32_GLOW");
        if (font == nullptr || m_glow == nullptr ||
            !m_boxes.load(device, services.unpackedRoot, services.strings)) {
            close();
            return false;
        }
        m_text.setFont(&m_font, font);
        // do_shop branches to state 20 for saved realm 8, level index 3 (H4).
        if (visit == ShopVisit::Level && levelName == "H4") {
            visit = ShopVisit::Completion;
        }
        m_session.start(party, results, maxima, classes, std::move(catalog), visit);
        // Decode the complete screen before exposing a transaction to the player.
        for (const auto& item : m_session.catalog().items()) {
            if (!item.texture.empty() && texture(item.texture) == nullptr) {
                throw FormatError("shop: missing icon " + item.texture);
            }
        }
        for (const auto* name :
             {"SHOP_SCROLL_1", "SHOP_SCROLL_2", "SHP_GOLD", "SHP_BONES", "SHP_EXP", "S1_BORDER",
              "S2_BORDER", "MORE_UP", "MORE_DOWN", "WINDOW_EMPTY"}) {
            if (texture(name) == nullptr) {
                throw FormatError(std::format("shop: missing artwork {}", name));
            }
        }
        for (s32 player = 0; player < 4; ++player) {
            texture(std::format("S1_PLYR{}", player + 1));
            texture(std::format("S2_PLYR{}", player + 1));
            m_scroll[static_cast<usize>(player)] =
                ShopLayout::make(m_session.catalog().items(), 0, m_font.height()).target;
            m_scrollSpeed[static_cast<usize>(player)] = kInitialScrollSpeed;
        }
        for (const auto& lane : m_session.lanes()) {
            texture(std::format("SHOP_TOP_{}", colorCode(lane.member.save.color)));
        }
        m_open = true;
    } catch (const std::exception& e) {
        log::warn("After-level screen: {}", e.what());
        close();
        return false;
    }
    // Confirmation and spoken-phrase durations participate in shop completion,
    // including on peers without an audio device. Music and tally loops do not.
    try {
        m_common.load(services.unpackedRoot / kSoundDirectory / "COMMON");
        loadVoices(party);
    } catch (const std::exception& e) {
        log::warn("Shop sound sources: {}", e.what());
    }
    if (m_services->sounds != nullptr) {
        try {
            const std::string bank = std::format("SHOP_{}", shopMusicRealm(levelName));
            if (m_musicBank.load(services.unpackedRoot / kSoundDirectory / bank)) {
                m_tallyCue = m_musicBank.find(std::format("S_TALLYSFX{}", bank.back()));
                if (const auto music = m_musicBank.find("S_" + bank); music.has_value()) {
                    m_music = m_services->sounds->play(m_musicBank.sequence(*music), 1,
                                                       SoundCategory::Music);
                }
            }
        } catch (const std::exception& e) {
            log::warn("Shop audio: {}", e.what());
        }
    }
    // The panel alone opens straight onto its cue; the rest follow the player's presses.
    for (const auto& event : m_session.takeEvents()) {
        playCue(event);
    }
    log::info("After-level tally/shop: {} players, {} catalog items", party.size(),
              m_session.catalog().items().size());
    return true;
}
void AfterLevelScene::loadVoices(std::span<const PartyMember> party) {
    m_narrator.load(m_services->unpackedRoot / kSoundDirectory / "VOICE1");
    m_narratorSecond.load(m_services->unpackedRoot / kSoundDirectory / "VOICE2");
    for (const auto& member : party) {
        // The unlockable classes speak with the voice of the class they shadow.
        const s32 named = member.save.character < kSumnerClass
                              ? member.save.character
                              : member.save.character % kStartingClassCount;
        auto& bank = m_voices[static_cast<usize>(std::clamp(member.player, 0, 3))];
        bank = SoundSet{};
        bank.load(m_services->unpackedRoot / kSoundDirectory / classCode(named));
    }
}
void AfterLevelScene::close() {
    if (m_services->sounds != nullptr) {
        for (const auto handle : m_outputEffects) {
            m_services->sounds->stop(handle);
        }
    }
    m_outputEffects.clear();
    if (m_services->sounds != nullptr && m_tallySound != kNoSound) {
        m_services->sounds->stop(m_tallySound);
    }
    m_tallySound = kNoSound;
    m_tallyCue.reset();
    if (m_services->sounds != nullptr && m_music != kNoSound) {
        m_services->sounds->stop(m_music);
    }
    m_music = kNoSound;
    m_effects.clear();
    m_announcement = Effect{};
    m_lastSounds.clear();
    m_pointerTargets.clear();
    m_hoverTargets.fill(std::nullopt);
    m_lastPointer.fill(std::nullopt);
    m_pointerTransform = Mat4{1};
    m_glow = nullptr;
    m_text.setFont(nullptr, nullptr);
    m_boxes.release();
    m_select.releaseTextures();
    m_inventory.releaseTextures();
    m_static.releaseTextures();
    m_device = nullptr;
    m_open = false;
}
std::string_view AfterLevelScene::text(std::string_view id) const {
    return m_services->strings != nullptr ? m_services->strings->get(id) : std::string_view{};
}
const Texture* AfterLevelScene::texture(std::string_view name) {
    for (auto* bank : {&m_select, &m_inventory, &m_static}) {
        if (const auto index = bank->find(name)) {
            return &bank->texture(*m_device, *index);
        }
    }
    return nullptr;
}
AfterLevelScene::Effect AfterLevelScene::effect(SoundSet& bank, std::string_view name, bool heard) {
    return effect(bank, name, Effect{}, heard);
}
AfterLevelScene::Effect AfterLevelScene::effect(SoundSet& bank, std::string_view name,
                                                const Effect& after, bool heard) {
    m_lastSounds.emplace_back(name);
    const auto index = bank.find(name);
    if (!index.has_value()) {
        return after;
    }
    SoundSequence sequence;
    try {
        sequence = bank.sequence(*index);
    } catch (const std::exception& e) {
        log::warn("Shop sound source {}: {}", name, e.what());
        return after;
    }
    Effect result = after;
    result.timing.append(SpeechTiming::of(sequence));
    if (result.timing.playing()) {
        m_effects.push_back(result.timing);
    }
    if (heard && m_services->sounds != nullptr) {
        try {
            result.handle = m_services->sounds->playAfter(after.handle, sequence);
            if (result.handle != kNoSound) {
                m_outputEffects.push_back(result.handle);
            }
        } catch (const std::exception& e) {
            log::warn("Shop sound playback {}: {}", name, e.what());
        }
    }
    return result;
}
void AfterLevelScene::advanceEffects(f64 seconds) {
    for (auto& effect : m_effects) {
        effect.advance(seconds);
    }
    m_announcement.timing.advance(seconds);
    std::erase_if(m_effects, [](const SpeechTiming& effect) { return !effect.playing(); });
    if (m_services->sounds != nullptr) {
        std::erase_if(m_outputEffects, [this](SoundHandle handle) {
            return !m_services->sounds->isPlaying(handle);
        });
    }
}
void AfterLevelScene::announceLevel(s32 player) {
    // AudioWithName: the character's name from its own bank (Pojo's while it carries him),
    // then "has" and "gained a level" from the narrator's.
    const auto lane = std::ranges::find(m_session.lanes(), player,
                                        [](const ShopLane& lane) { return lane.member.player; });
    if (lane == m_session.lanes().end()) {
        return;
    }
    const CharacterSave& save = lane->member.save;
    // Sumner speaks of one player at a time: a second announcement waits for the first to
    // end, in the order the players reached their level panels.
    Effect after = m_announcement;
    if (PickupVoices::carriesPojo(save)) {
        after = effect(m_narratorSecond.find(kPojoName) ? m_narratorSecond : m_narrator, kPojoName,
                       after);
    } else {
        after = effect(m_voices[static_cast<usize>(std::clamp(player, 0, 3))],
                       PickupVoices::nameOf(save.character, save.color), after);
    }
    for (const std::string_view name : {kHasSound, kGainedLevelSound}) {
        after = effect(m_narratorSecond.find(name) ? m_narratorSecond : m_narrator, name, after);
    }
    m_announcement = after;
}
void AfterLevelScene::playCue(const ShopEvent& event) {
    switch (event.cue) {
    case ShopCue::Select: effect(m_common, kSelectSound, m_cuesHeard); break;
    case ShopCue::CursorNext: effect(m_common, kCursorNextSound, m_cuesHeard); break;
    case ShopCue::CursorPrevious: effect(m_common, kCursorPreviousSound, m_cuesHeard); break;
    case ShopCue::Bought:
    case ShopCue::Sold: effect(m_common, kTradeSound, m_cuesHeard); break;
    case ShopCue::Refused: effect(m_common, kRefusedSound, m_cuesHeard); break;
    case ShopCue::InventoryShown: effect(m_common, kInventorySound, m_cuesHeard); break;
    case ShopCue::LevelGained: announceLevel(event.player); break;
    }
}
bool AfterLevelScene::update(f64 seconds, const ShopSession::Inputs& inputs,
                             const std::array<s32, 4>* potions, ShopSession::Inputs* accepted,
                             bool committed) {
    if (!m_open || !std::isfinite(seconds) || seconds < 0) {
        return false;
    }
    advanceEffects(seconds);
    m_lastSounds.clear();
    m_time = std::fmod(m_time + seconds, 85.0 / 60.0);
    m_frameTime = std::fmod(m_frameTime + seconds, 2.5);
    std::array<bool, 4> wasScrolling{};
    auto heard = inputs;
    for (const auto& lane : m_session.lanes()) {
        const auto player = static_cast<usize>(lane.member.player);
        if (!committed && lane.phase == ShopPhase::Shopping && m_services->config != nullptr) {
            heard[player] = shopInput(heard[player], *m_services->config, lane.member.player);
        }
        const auto layout =
            ShopLayout::make(m_session.catalog().items(), lane.cursor, m_font.height());
        wasScrolling[player] = m_scroll[player] != layout.target;
        // do_shopping reads buy/sell edges only when the prior scroll speed is zero.
        // Discard the edge rather than queueing a transaction for a different row.
        if (!committed && lane.phase == ShopPhase::Shopping && wasScrolling[player]) {
            heard[player].select = false;
            heard[player].back = false;
        }
    }
    if (!committed) {
        pointerInput(heard, wasScrolling);
    }
    if (accepted != nullptr) {
        *accepted = heard;
    }
    m_session.update(seconds, heard, potions);
    updateTallySound();
    for (const auto& lane : m_session.lanes()) {
        const auto player = static_cast<usize>(lane.member.player);
        const auto layout =
            ShopLayout::make(m_session.catalog().items(), lane.cursor, m_font.height());
        if (lane.scrollJump) {
            m_scroll[player] = layout.target;
            m_scrollSpeed[player] = kInitialScrollSpeed;
        } else {
            if (wasScrolling[player]) {
                m_scrollSpeed[player] += lane.navigationSteps;
            } else {
                m_scrollSpeed[player] = kInitialScrollSpeed;
            }
            const f32 step = static_cast<f32>(std::min(seconds, 60.0)) * 60 *
                             static_cast<f32>(m_scrollSpeed[player]);
            m_scroll[player] += std::clamp(layout.target - m_scroll[player], -step, step);
            if (m_scroll[player] == layout.target) {
                m_scrollSpeed[player] = kInitialScrollSpeed;
            }
        }
    }
    for (const auto& event : m_session.takeEvents()) {
        playCue(event);
    }
    // Retain do_shop's final-effect wait, but measure native clip durations in
    // simulation time. A slow, absent or failed output device cannot hold travel.
    return m_session.finished() && m_effects.empty();
}

std::optional<usize> AfterLevelScene::captureOnlinePointer(s32 seat, MenuInput& input) {
    if (!m_open || seat < 0 || seat >= 4) {
        return std::nullopt;
    }
    std::array<bool, 4> scrolling{};
    for (const auto& lane : m_session.lanes()) {
        const auto player = static_cast<usize>(lane.member.player);
        const auto layout =
            ShopLayout::make(m_session.catalog().items(), lane.cursor, m_font.height());
        scrolling[player] = m_scroll[player] != layout.target;
    }
    ShopSession::Inputs inputs{};
    inputs[static_cast<usize>(seat)] = input;
    std::array<s32, 4> focus{-1, -1, -1, -1};
    pointerInput(inputs, scrolling, &focus, seat);
    input = std::move(inputs[static_cast<usize>(seat)]);
    const auto row = focus[static_cast<usize>(seat)];
    return row < 0 ? std::nullopt : std::optional{static_cast<usize>(row)};
}

bool AfterLevelScene::abandonLane(s32 seat) {
    if (!m_open) {
        return false;
    }
    m_session.abandon(seat);
    return true;
}
bool AfterLevelScene::focusOnline(s32 seat, usize row) {
    return m_open && m_session.focus(seat, row);
}

Rect AfterLevelScene::laneArea(const ShopLane& lane) {
    return {static_cast<f32>(lane.member.player * StatusBoxPainter::kWidth), 0,
            StatusBoxPainter::kWidth, StatusBoxPainter::kY};
}
void AfterLevelScene::pointerInput(ShopSession::Inputs& inputs,
                                   const std::array<bool, 4>& scrolling, std::array<s32, 4>* focus,
                                   s32 onlyPlayer) {
    for (const auto& lane : m_session.lanes()) {
        if (onlyPlayer >= 0 && lane.member.player != onlyPlayer) {
            continue;
        }
        const auto player = static_cast<usize>(lane.member.player);
        const bool rightClick = inputs[player].pointerBack;
        inputs[player].pointerBack = false; // Sell needs a row, not just the lane's background.
        auto& input = inputs[player];
        input = mapMenuPointer(std::move(input), m_pointerTransform, laneArea(lane));
        // A pointer chooses where it appears and once it has moved a little, never while
        // it only rests where the rows scroll under it.
        const bool moved = input.pointer && pointerMoved(m_lastPointer[player], *input.pointer,
                                                         OptionMenu::kHoverMotion);
        if (moved) {
            m_lastPointer[player] = input.pointer;
        } else if (!input.pointer) {
            m_lastPointer[player].reset();
        }
        const bool keyboard = input.up || input.down || input.left || input.right || input.select ||
                              input.back || input.start || input.upHeld || input.downHeld ||
                              input.leftHeld || input.rightHeld;
        if (!input.pointer || keyboard || lane.phase == ShopPhase::Done) {
            m_hoverTargets[player].reset();
            continue;
        }
        const bool active = moved || input.pointerPressed || rightClick || input.pointerScroll != 0;
        const auto* target = m_pointerTargets.at(*input.pointer, [&](const auto& shown) {
            return shown.control.seat == lane.member.player && shown.control.phase == lane.phase;
        });
        if (!active) {
            if (target == nullptr || m_hoverTargets[player] != target->control) {
                m_hoverTargets[player].reset();
            }
            continue;
        }
        if (lane.phase == ShopPhase::Shopping && input.pointerScroll != 0) {
            input.up = input.pointerScroll > 0;
            input.down = input.pointerScroll < 0;
            m_hoverTargets[player].reset();
            continue;
        }
        if (target == nullptr) {
            m_hoverTargets[player].reset();
            continue;
        }
        const ShopControl& hit = target->control;
        m_hoverTargets[player] = hit;
        switch (hit.action) {
        case PointerAction::Buy:
        case PointerAction::Sell: {
            const auto& catalog = m_session.catalog().items();
            const bool selectable = hit.row < catalog.size() && lane.selectable(catalog[hit.row]);
            bool focused = false;
            if (focus != nullptr) {
                if (selectable && lane.phase == ShopPhase::Shopping) {
                    (*focus)[player] = static_cast<s32>(hit.row);
                    focused = true;
                }
            } else {
                focused = m_session.focus(lane.member.player, hit.row);
            }
            if (focused && !scrolling[player]) {
                input.back =
                    (rightClick || (input.pointerPressed && hit.action == PointerAction::Sell));
                input.select = input.pointerPressed && !input.back;
            }
            break;
        }
        case PointerAction::Previous: input.up = input.pointerPressed; break;
        case PointerAction::Next: input.down = input.pointerPressed; break;
        case PointerAction::Continue: input.select = input.pointerPressed; break;
        }
    }
}
void AfterLevelScene::updateTallySound() {
    if (m_services->sounds == nullptr || !m_tallyCue.has_value()) {
        return;
    }
    const bool growing = std::ranges::any_of(m_session.lanes(), [](const ShopLane& lane) {
        return lane.phase == ShopPhase::Tally && !lane.tally.finished();
    });
    // One shared realm-specific loop, not a new voice per pile or player.
    if (!growing) {
        if (m_tallySound != kNoSound) {
            m_services->sounds->stop(m_tallySound);
            m_tallySound = kNoSound;
        }
    } else if (!m_services->sounds->isPlaying(m_tallySound)) {
        try {
            m_tallySound = m_services->sounds->play(m_musicBank.sequence(*m_tallyCue));
        } catch (const std::exception& e) {
            log::warn("Shop tally audio: {}", e.what());
            m_tallyCue.reset();
        }
    }
}
} // namespace gdl::game
