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

#include "game/screens/ShopLayout.h"
#include "game/screens/ShopMusic.h"

namespace gdl::game {
bool AfterLevelScene::open(RenderDevice& device, const GameContext& context,
                           std::span<const PartyMember> party,
                           std::span<const LevelResults> results, const std::array<s32, 3>& maxima,
                           std::string_view levelName, bool towerShop) {
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
        const auto* font = texture("FONT32");
        m_glow = texture("FONT32_GLOW");
        if (font == nullptr || m_glow == nullptr ||
            !m_boxes.load(device, context.unpackedRoot, context.strings)) {
            close();
            return false;
        }
        m_text.setFont(&m_font, font);
        m_session.start(party, results, maxima, classes, std::move(catalog));
        if (towerShop) {
            m_session.skipTally();
        }
        // Decode the complete screen before exposing a transaction to the player.
        for (const auto& item : m_session.catalog().items()) {
            if (!item.texture.empty() && texture(item.texture) == nullptr) {
                throw FormatError("shop: missing icon " + item.texture);
            }
        }
        for (const auto* name :
             {"SHOP_SCROLL_1", "SHOP_SCROLL_2", "SHP_GOLD", "SHP_BONES", "SHP_EXP", "S1_BORDER",
              "S2_BORDER", "BUTTON_X", "MORE_UP", "MORE_DOWN"}) {
            if (texture(name) == nullptr) {
                throw FormatError(std::format("shop: missing artwork {}", name));
            }
        }
        for (s32 player = 0; player < 4; ++player) {
            texture(std::format("S1_PLYR{}", player + 1));
            texture(std::format("S2_PLYR{}", player + 1));
            m_scroll[static_cast<usize>(player)] =
                ShopLayout::make(m_session.catalog().items(), 0, m_font.height()).target;
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
            m_common.load(context.unpackedRoot / "audio/COMMON");
            const std::string bank = std::format("SHOP_{}", shopMusicRealm(levelName));
            if (m_musicBank.load(context.unpackedRoot / "audio" / bank)) {
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
    log::info("After-level tally/shop: {} players, {} catalog items", party.size(),
              m_session.catalog().items().size());
    return true;
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
void AfterLevelScene::sound(std::string_view name) {
    if (m_context.sounds == nullptr) {
        return;
    }
    if (const auto index = m_common.find(name); index.has_value()) {
        m_context.sounds->play(m_common.sequence(*index));
    }
}
bool AfterLevelScene::update(f64 seconds, const ShopSession::Inputs& inputs) {
    if (!m_open || !std::isfinite(seconds) || seconds < 0) {
        return false;
    }
    m_time = std::fmod(m_time + seconds, 85.0 / 60.0);
    m_session.update(seconds, inputs);
    updateTallySound();
    for (const auto& lane : m_session.lanes()) {
        const auto player = static_cast<usize>(lane.member.player);
        const auto& input = inputs[player];
        const auto layout =
            ShopLayout::make(m_session.catalog().items(), lane.cursor, m_font.height());
        const f32 step = static_cast<f32>(std::min(seconds, 60.0)) * 120;
        m_scroll[player] += std::clamp(layout.target - m_scroll[player], -step, step);
        if (lane.transacted) {
            sound(lane.feedback == ShopResult::Bought || lane.feedback == ShopResult::Sold ||
                          lane.feedback == ShopResult::Exit
                      ? "S_OPTMENUSEL"
                      : "S_NO");
        } else if (lane.phase == ShopPhase::Shopping &&
                   (input.up || input.down || input.left || input.right)) {
            sound("S_OPTMENUMOVVRT");
        }
    }
    return m_session.finished();
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
