#include "game/screens/AfterLevelScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <string>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/players/PickupVoices.h"
#include "game/screens/ShopLayout.h"
#include "game/screens/ShopMusic.h"

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
} // namespace
bool AfterLevelScene::open(RenderDevice& device, const GameContext& context,
                           std::span<const PartyMember> party,
                           std::span<const LevelResults> results, const std::array<s32, 3>& maxima,
                           std::string_view levelName, ShopVisit visit) {
    close();
    m_context = context;
    m_device = &device;
    m_time = 0;
    try {
        ShopCatalog catalog;
        ClassDataSet classes;
        if (!catalog.load(context.unpackedRoot / "shop/catalog.json") ||
            !classes.load(context.unpackedRoot / "pdata") ||
            !m_select.load(context.unpackedRoot / "SELECT") ||
            !m_inventory.load(context.unpackedRoot / "INVENTORY") ||
            !m_static.load(context.unpackedRoot / "STATIC") ||
            !m_font.load(context.unpackedRoot / "fonts/font32.json", 16) ||
            !m_titles.load(context.unpackedRoot / "text/english.json")) {
            close();
            return false;
        }
        if (context.strings != nullptr) {
            m_titles.translate(*context.strings, kHelpTextPrefix);
        }
        const auto* font = texture("FONT32");
        m_glow = texture("FONT32_GLOW");
        if (font == nullptr || m_glow == nullptr ||
            !m_boxes.load(device, context.unpackedRoot, context.strings)) {
            close();
            return false;
        }
        m_text.setFont(&m_font, font);
        m_session.start(party, results, maxima, classes, std::move(catalog), visit);
        // Decode the complete screen before exposing a transaction to the player.
        for (const auto& item : m_session.catalog().items()) {
            if (!item.texture.empty() && texture(item.texture) == nullptr) {
                throw FormatError("shop: missing icon " + item.texture);
            }
        }
        for (const auto* name :
             {"SHOP_SCROLL_1", "SHOP_SCROLL_2", "SHP_GOLD", "SHP_BONES", "SHP_EXP", "S1_BORDER",
              "S2_BORDER", "BUTTON_X", "MORE_UP", "MORE_DOWN", "WINDOW_EMPTY"}) {
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
    if (m_context.sounds != nullptr) {
        try {
            m_common.load(context.unpackedRoot / kSoundDirectory / "COMMON");
            loadVoices(party);
            const std::string bank = std::format("SHOP_{}", shopMusicRealm(levelName));
            if (m_musicBank.load(context.unpackedRoot / kSoundDirectory / bank)) {
                m_tallyCue = m_musicBank.find(std::format("S_TALLYSFX{}", bank.back()));
                if (const auto music = m_musicBank.find("S_" + bank); music.has_value()) {
                    m_music = m_context.sounds->play(m_musicBank.sequence(*music), 1,
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
    m_narrator.load(m_context.unpackedRoot / kSoundDirectory / "VOICE1");
    m_narratorSecond.load(m_context.unpackedRoot / kSoundDirectory / "VOICE2");
    for (const auto& member : party) {
        // The unlockable classes speak with the voice of the class they shadow.
        const s32 named = member.save.character < kSumnerClass
                              ? member.save.character
                              : member.save.character % kStartingClassCount;
        auto& bank = m_voices[static_cast<usize>(std::clamp(member.player, 0, 3))];
        bank = SoundSet{};
        bank.load(m_context.unpackedRoot / kSoundDirectory / classCode(named));
    }
}
void AfterLevelScene::close() {
    if (m_context.sounds != nullptr && m_tallySound != kNoSound) {
        m_context.sounds->stop(m_tallySound);
    }
    m_tallySound = kNoSound;
    m_tallyCue.reset();
    if (m_context.sounds != nullptr && m_music != kNoSound) {
        m_context.sounds->stop(m_music);
    }
    m_music = kNoSound;
    m_effects.clear();
    m_lastSounds.clear();
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
    return m_context.strings != nullptr ? m_context.strings->get(id) : std::string_view{};
}
const Texture* AfterLevelScene::texture(std::string_view name) {
    for (auto* bank : {&m_select, &m_inventory, &m_static}) {
        if (const auto index = bank->find(name)) {
            return &bank->texture(*m_device, *index);
        }
    }
    return nullptr;
}
SoundHandle AfterLevelScene::effect(SoundSet& bank, std::string_view name, SoundHandle after) {
    m_lastSounds.emplace_back(name);
    if (m_context.sounds == nullptr) {
        return after;
    }
    const auto index = bank.find(name);
    if (!index.has_value()) {
        return after;
    }
    try {
        const SoundHandle handle = m_context.sounds->playAfter(after, bank.sequence(*index));
        if (handle != kNoSound) {
            m_effects.push_back(handle);
        }
        return handle;
    } catch (const std::exception& e) {
        log::warn("Shop sound {}: {}", name, e.what());
        return after;
    }
}
bool AfterLevelScene::effectsPlaying() {
    if (m_context.sounds == nullptr) {
        m_effects.clear();
        return false;
    }
    std::erase_if(m_effects,
                  [this](SoundHandle handle) { return !m_context.sounds->isPlaying(handle); });
    return !m_effects.empty();
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
    SoundHandle after = kNoSound;
    if (PickupVoices::carriesPojo(save)) {
        after = effect(m_narratorSecond.find(kPojoName) ? m_narratorSecond : m_narrator, kPojoName);
    } else {
        after = effect(m_voices[static_cast<usize>(std::clamp(player, 0, 3))],
                       PickupVoices::nameOf(save.character, save.color));
    }
    for (const std::string_view name : {kHasSound, kGainedLevelSound}) {
        after = effect(m_narratorSecond.find(name) ? m_narratorSecond : m_narrator, name, after);
    }
}
void AfterLevelScene::playCue(const ShopEvent& event) {
    switch (event.cue) {
    case ShopCue::Select: effect(m_common, kSelectSound); break;
    case ShopCue::CursorNext: effect(m_common, kCursorNextSound); break;
    case ShopCue::CursorPrevious: effect(m_common, kCursorPreviousSound); break;
    case ShopCue::Bought:
    case ShopCue::Sold: effect(m_common, kTradeSound); break;
    case ShopCue::Refused: effect(m_common, kRefusedSound); break;
    case ShopCue::InventoryShown: effect(m_common, kInventorySound); break;
    case ShopCue::LevelGained: announceLevel(event.player); break;
    }
}
bool AfterLevelScene::update(f64 seconds, const ShopSession::Inputs& inputs) {
    if (!m_open || !std::isfinite(seconds) || seconds < 0) {
        return false;
    }
    m_lastSounds.clear();
    m_time = std::fmod(m_time + seconds, 85.0 / 60.0);
    std::array<bool, 4> wasScrolling{};
    auto heard = inputs;
    for (const auto& lane : m_session.lanes()) {
        const auto player = static_cast<usize>(lane.member.player);
        const auto layout =
            ShopLayout::make(m_session.catalog().items(), lane.cursor, m_font.height());
        wasScrolling[player] = m_scroll[player] != layout.target;
        // do_shopping reads buy/sell edges only when the prior scroll speed is zero.
        // Discard the edge rather than queueing a transaction for a different row.
        if (lane.phase == ShopPhase::Shopping && wasScrolling[player]) {
            heard[player].select = false;
            heard[player].back = false;
        }
    }
    m_session.update(seconds, heard);
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
    // The original leaves the shop only once its effects have died away (do_shop's
    // sndFxUpdate test), so the last confirmation is heard whole.
    return m_session.finished() && !effectsPlaying();
}
void AfterLevelScene::updateTallySound() {
    if (m_context.sounds == nullptr || !m_tallyCue.has_value()) {
        return;
    }
    const bool growing = std::ranges::any_of(m_session.lanes(), [](const ShopLane& lane) {
        return lane.phase == ShopPhase::Tally && !lane.tally.finished();
    });
    // One shared realm-specific loop, not a new voice per pile or player.
    if (!growing) {
        if (m_tallySound != kNoSound) {
            m_context.sounds->stop(m_tallySound);
            m_tallySound = kNoSound;
        }
    } else if (!m_context.sounds->isPlaying(m_tallySound)) {
        try {
            m_tallySound = m_context.sounds->play(m_musicBank.sequence(*m_tallyCue));
        } catch (const std::exception& e) {
            log::warn("Shop tally audio: {}", e.what());
            m_tallyCue.reset();
        }
    }
}
} // namespace gdl::game
