#include "game/screens/LevelMessages.h"

#include <exception>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr std::string_view kStaticDirectory = "STATIC";
constexpr std::string_view kFontFile = "fonts/font32.json";
constexpr s32 kFont32SpaceWidth = 16;
constexpr std::string_view kFontTexture = "FONT32";
constexpr std::string_view kGlowTexture = "FONT32_GLOW";
constexpr std::string_view kScrollTexture = "SCROLL_A";
constexpr std::string_view kFireRingTexture = "GREENCIRCTRANS";
constexpr std::string_view kFireMaskTexture = "GREENCIRCTRANSM";
constexpr std::string_view kScrollTextFile = "text/scroll_e.json";
constexpr std::string_view kScrollTextPrefix = "scroll";
} // namespace

void LevelMessages::load(RenderDevice& device, TextureSet& textures,
                         const std::filesystem::path& root, const StringTable* strings) {
    clear();
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
    const auto texture = [&](std::string_view name, u32 frame = 0) -> const Texture* {
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
    const auto scroll = textures.find(kScrollTexture);
    const auto ring = textures.find(kFireRingTexture);
    const auto mask = textures.find(kFireMaskTexture);
    if (scroll.has_value() && ring.has_value() && mask.has_value()) {
        const u32 firstRing = *ring;
        const u32 firstMask = *mask;
        try {
            art.backdropImage = &textures.image(*scroll);
            const auto frames = static_cast<u32>(BurnDialogueScroll::kFrameCount);
            for (u32 i = 1; i <= frames && firstRing + i < textures.size(); ++i) {
                art.burnRing.push_back(&textures.texture(device, firstRing + i));
            }
            for (u32 i = 1; i <= frames && firstMask + i < textures.size(); ++i) {
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
    m_message.reset();
    m_firstPage = 0;
    m_replicaPages.clear();
    m_replicaPrompt.clear();
    m_controlLabels = {};
    m_scroll.close();
    m_scroll.setArt({});
    m_scroll.setText(nullptr);
    m_text.setFont(nullptr, nullptr);
}

bool LevelMessages::open(RenderDevice& device, std::string_view name, const StringTable* strings,
                         std::optional<usize> page) {
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
    const bool opened = m_scroll.open(
        device, page.has_value() ? std::vector<std::string>{message.pages[*page]} : message.pages,
        message.scale,
        strings != nullptr ? std::string(strings->get("scroll.pressButton")) : std::string{});
    if (opened) {
        m_message = found;
        m_firstPage = static_cast<u32>(page.value_or(0));
    }
    return opened;
}

std::optional<LevelMessages::Look> LevelMessages::look() const {
    if (!active() || !m_message) {
        return std::nullopt;
    }
    return Look{*m_message, m_firstPage + static_cast<u32>(m_scroll.page()), m_scroll.burnFrame(),
                m_scroll.promptAlpha()};
}

bool LevelMessages::preloadReplica(RenderDevice& device, const StringTable* strings) {
    m_replicaPages.clear();
    if (!m_text.ready() || !m_scrollText.loaded()) {
        return false;
    }
    m_replicaPrompt =
        strings != nullptr ? std::string(strings->get("scroll.pressButton")) : std::string{};
    m_scroll.preloadBurn(device);
    m_replicaPages.reserve(m_scrollText.size());
    for (usize i = 0; i < m_scrollText.size(); ++i) {
        const auto& message = m_scrollText.message(static_cast<u32>(i));
        std::vector<ScrollBox::PageLayout> pages;
        pages.reserve(message.pages.size());
        for (const auto& page : message.pages) {
            pages.push_back(m_scroll.layout(page, message.scale, m_replicaPrompt));
        }
        m_replicaPages.push_back(std::move(pages));
    }
    return m_scroll.acceptsFrame(-1);
}

bool LevelMessages::accepts(const Look& look) const {
    return look.message < m_replicaPages.size() &&
           look.page < m_replicaPages[look.message].size() && m_scroll.acceptsFrame(look.burnFrame);
}

void LevelMessages::drawReplica(Canvas& canvas, const Look& look) const {
    if (accepts(look)) {
        m_scroll.drawPage(canvas, m_replicaPages[look.message][look.page],
                          m_scrollText.message(look.message).scale, m_replicaPrompt,
                          look.promptAlpha, look.burnFrame);
    }
}

LevelMessages::Cues LevelMessages::step(s32 ticks, u32 accepted) {
    if (!m_scroll.active()) {
        return {};
    }
    const bool wasBurning = m_scroll.burning();
    m_scroll.step(ticks, accepted);
    const bool stopVoice = (m_scroll.burning() && !wasBurning) || !m_scroll.active();
    return {stopVoice, stopVoice && !wasBurning};
}

} // namespace gdl::game
