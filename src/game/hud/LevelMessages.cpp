#include "game/hud/LevelMessages.h"

#include <exception>
#include <sstream>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/widgets/MenuFont.h"

namespace gdl::game {
namespace {
constexpr std::string_view kStaticDirectory = "STATIC";
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
        !loadMenuFont(m_font32, root, kFont32SpaceWidth) ||
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
    m_localBinding = std::make_shared<const u8>(0);
    m_sim.message.reset();
    m_sim.firstPage = 0;
    setPresentation(nullptr);
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
        m_sim.message = found;
        m_sim.firstPage = static_cast<u32>(page.value_or(0));
    }
    return opened;
}

std::optional<LevelMessages::Look> LevelMessages::look() const {
    if (!active() || !m_sim.message) {
        return std::nullopt;
    }
    return Look{*m_sim.message, m_sim.firstPage + static_cast<u32>(m_scroll.page()),
                m_scroll.burnFrame(), m_scroll.promptAlpha()};
}

bool LevelMessages::openImported(RenderDevice& device, std::span<const std::string> text,
                                 const StringTable* strings) {
    const auto& painter = m_presentation ? m_presentation->text() : m_text;
    if (!painter.ready() || text.empty() || active()) {
        return false;
    }
    const f32 scale = m_presentation ? m_presentation->scale() : .5f;
    const s32 width =
        m_presentation ? m_presentation->textWidth() : ScrollBox::kMaxWidth - ScrollBox::kMargin;
    const usize linesPerPage = m_presentation ? m_presentation->linesPerPage() : 9;
    std::vector<std::string> pages;
    std::string page;
    usize lines = 0;
    const auto append = [&](const std::string& line) {
        if (lines != 0) {
            page += '\n';
        }
        page += line;
        if (++lines == linesPerPage) {
            pages.push_back(std::exchange(page, {}));
            lines = 0;
        }
    };
    for (const auto& document : text) {
        std::istringstream paragraphs{document};
        std::string paragraph;
        while (std::getline(paragraphs, paragraph)) {
            std::istringstream words{paragraph};
            std::string word;
            std::string line;
            while (words >> word) {
                if (!line.empty() &&
                    painter.measure(std::string(line).append(" ").append(word), scale) > width) {
                    append(line);
                    line.clear();
                }
                if (!line.empty()) {
                    line += ' ';
                }
                // Malformed overlong tokens still cannot draw outside the parchment.
                for (const char character : word) {
                    if (!line.empty() && painter.measure(line + character, scale) > width) {
                        append(line);
                        line.clear();
                    }
                    line += character;
                }
            }
            if (!line.empty()) {
                append(line);
            }
        }
        if (!page.empty()) {
            pages.push_back(std::exchange(page, {}));
            lines = 0;
        }
    }
    if (pages.empty()) {
        return false;
    }
    const bool opened =
        m_scroll.open(device, std::move(pages), scale,
                      strings ? std::string(strings->get("scroll.pressButton")) : std::string{});
    if (opened) {
        m_sim.message.reset();
        m_sim.firstPage = 0;
    }
    return opened;
}

bool LevelMessages::openStyled(RenderDevice& device, std::string_view style,
                               std::vector<std::string> pages, const StringTable* strings) {
    if (!m_scrollText.loaded() || pages.empty() || active()) {
        return false;
    }
    const auto found = m_scrollText.find(style);
    if (!found.has_value()) {
        log::warn("Tower: no message {} to draw a scroll as", style);
        return false;
    }
    const bool opened = m_scroll.open(
        device, std::move(pages), m_scrollText.message(*found).scale,
        strings != nullptr ? std::string(strings->get("scroll.pressButton")) : std::string{});
    if (opened) {
        // None of the rom's messages, so nothing for look() to name.
        m_sim.message.reset();
        m_sim.firstPage = 0;
    }
    return opened;
}

LevelMessages::Cues LevelMessages::step(s32 ticks, u32 accepted) {
    if (!m_scroll.active()) {
        return {};
    }
    const bool wasBurning = m_scroll.burning();
    m_scroll.step(ticks, accepted);
    const bool stopVoice = (m_scroll.burning() && !wasBurning) || !m_scroll.active();
    return {stopVoice, m_presentation == nullptr && stopVoice && !wasBurning};
}

void LevelMessages::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    m_scroll.saveLocal(saved.m_scroll);
}

bool LevelMessages::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && saved.m_binding.lock() == m_localBinding &&
           m_scroll.canRestoreLocal(saved.m_scroll);
}

bool LevelMessages::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    m_scroll.restoreLocal(saved.m_scroll);
    return true;
}

bool LevelMessages::appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    digest.string("level-messages-native-v1");
    digest.values(m_font32, m_text, m_scrollText);
    digest.fields<2>(saved ? saved->m_state : m_sim);
    if (saved) {
        m_scroll.appendLocalDigest(digest, saved->m_scroll);
    } else {
        m_scroll.appendNativeDigest(digest);
    }
    return digest.good();
}

bool LevelMessages::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, nullptr);
}

bool LevelMessages::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, &saved);
}

} // namespace gdl::game
