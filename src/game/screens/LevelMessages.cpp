#include "game/screens/LevelMessages.h"

#include <cstddef>
#include <exception>
#include <utility>

#include "engine/core/Log.h"

namespace gdl::game {
namespace {
constexpr std::string_view kStaticDirectory = "STATIC";
constexpr std::string_view kFontFile = "fonts/font32.json";
constexpr int kFont32SpaceWidth = 16;
constexpr std::string_view kFontTexture = "FONT32";
constexpr std::string_view kGlowTexture = "FONT32_GLOW";
constexpr std::string_view kScrollTexture = "SCROLL_A";
constexpr std::string_view kButtonTexture = "BUTTON_TRI";
constexpr std::string_view kFireRingTexture = "GREENCIRCTRANS";
constexpr std::string_view kFireMaskTexture = "GREENCIRCTRANSM";
constexpr std::string_view kScrollTextFile = "text/scroll_e.json";
constexpr std::string_view kPromptText = "scroll.pressButton";
constexpr std::string_view kScrollTextPrefix = "scroll";
} // namespace

void LevelMessages::load(RenderDevice& device, TextureSet& textures,
                         const std::filesystem::path& root, const StringTable* strings) {
    ScrollBoxArt art;
    m_text.setFont(nullptr, nullptr);
    if (!textures.load(root / kStaticDirectory) ||
        !m_font32.load(root / kFontFile, kFont32SpaceWidth) ||
        !m_scrollText.load(root / kScrollTextFile)) {
        log::warn("Tower: the scroll's art or texts are not unpacked; the welcome is skipped");
        m_scroll.setArt(art);
        return;
    }
    if (strings != nullptr) {
        m_scrollText.translate(*strings, kScrollTextPrefix);
    }
    const auto texture = [&](std::string_view name, unsigned int frame = 0) -> const Texture* {
        const auto index = textures.find(name);
        if (!index.has_value() || *index + frame >= textures.size()) {
            return nullptr;
        }
        try {
            return &textures.texture(device, *index + frame);
        } catch (const std::exception& e) {
            log::warn("Tower: texture {}: {}", name, e.what());
            return nullptr;
        }
    };
    const Texture* font = texture(kFontTexture);
    if (font != nullptr) {
        m_text.setFont(&m_font32, font);
    }
    art.backdrop = texture(kScrollTexture);
    art.glow = texture(kGlowTexture);
    art.button = texture(kButtonTexture);
    const auto scroll = textures.find(kScrollTexture);
    const auto ring = textures.find(kFireRingTexture);
    const auto mask = textures.find(kFireMaskTexture);
    if (scroll.has_value() && ring.has_value() && mask.has_value()) {
        const unsigned int firstRing = *ring;
        const unsigned int firstMask = *mask;
        try {
            art.backdropImage = &textures.image(*scroll);
            const auto frames = static_cast<unsigned int>(BurnDialogueScroll::kFrameCount);
            for (unsigned int i = 1; i <= frames && firstRing + i < textures.size(); ++i) {
                art.burnRing.push_back(&textures.texture(device, firstRing + i));
            }
            for (unsigned int i = 1; i <= frames && firstMask + i < textures.size(); ++i) {
                art.burnMasks.push_back(&textures.image(firstMask + i));
            }
        } catch (const std::exception& e) {
            log::warn("Tower: burn frames: {}", e.what());
            art.backdropImage = nullptr;
            art.burnRing.clear();
            art.burnMasks.clear();
        }
    }
    m_scroll.setText(&m_text);
    m_scroll.setArt(std::move(art));
}

void LevelMessages::clear() {
    m_scroll.close();
    m_scroll.setArt({});
    m_scroll.setText(nullptr);
    m_text.setFont(nullptr, nullptr);
}

bool LevelMessages::open(RenderDevice& device, std::string_view name, const StringTable* strings,
                         std::optional<std::size_t> page) {
    if (!m_scrollText.loaded()) {
        return false;
    }
    const auto found = m_scrollText.find(name);
    if (!found.has_value()) {
        if (page.has_value()) {
            log::warn("Tower: no page {} of the message {}", *page, name);
        }
        return false;
    }
    const MessageInfo& message = m_scrollText.message(*found);
    if (page.has_value() && *page >= message.pages.size()) {
        log::warn("Tower: no page {} of the message {}", *page, name);
        return false;
    }
    const std::string prompt = strings != nullptr ? std::string(strings->get(kPromptText)) : "";
    return m_scroll.open(
        device, page.has_value() ? std::vector<std::string>{message.pages[*page]} : message.pages,
        message.scale, prompt);
}

LevelMessages::Cues LevelMessages::step(int ticks, unsigned int accepted) {
    if (!m_scroll.active()) {
        return {};
    }
    const bool wasBurning = m_scroll.burning();
    m_scroll.step(ticks, accepted);
    const bool stopVoice = (m_scroll.burning() && !wasBurning) || !m_scroll.active();
    return {stopVoice, stopVoice && !wasBurning};
}

} // namespace gdl::game
