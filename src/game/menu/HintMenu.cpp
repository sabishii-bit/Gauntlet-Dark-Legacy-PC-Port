#include "game/menu/HintMenu.h"

#include <utility>

namespace gdl::game {

namespace {

constexpr std::string_view kBackdrop = "SCROLL_A";

/** What the list and its pages share: the scroll, the ink, the prompts' row and the tag. */
MenuDefinition scrollDefinition(const HintMenuLabels& labels) {
    MenuDefinition menu;
    menu.y = -1;
    menu.colors.off = HintMenu::kInk;
    menu.prompts = true;
    menu.backLabel = labels.back;
    menu.promptY = HintMenu::kPromptY;
    menu.fades = true;
    menu.parchmentFont = true;
    menu.playerLabel = labels.player;
    menu.backdrop = std::string(kBackdrop);
    menu.backdropX = -1;
    menu.backdropY = HintMenu::kBackdropY;
    menu.backdropWidth = HintMenu::kBackdropWidth;
    menu.backdropHeight = HintMenu::kBackdropHeight;
    return menu;
}

} // namespace

MenuDefinition HintMenu::topicsDefinition(bool fadeBackdrop) const {
    MenuDefinition menu = scrollDefinition(m_labels);
    menu.title = m_labels.title;
    menu.titleScale = kTitleScale;
    menu.x = kTopicsX;
    menu.items = m_labels.topics;
    menu.selectLabel = m_labels.select;
    menu.garamondIntro = true;
    menu.backdropFades = fadeBackdrop;
    return menu;
}

bool HintMenu::open(const TextPainter& painter, const MenuScreen& screen, HintMenuLabels labels) {
    close();
    if (!painter.ready() || labels.topics.empty()) {
        return false;
    }
    m_labels = std::move(labels);
    m_screen = screen;
    m_painter = &painter;
    m_topics.open(topicsDefinition(true), painter, screen);
    return true;
}

void HintMenu::read(const TextPainter& painter, std::string title,
                    std::vector<std::string> passages, float scale, bool centred, int gap) {
    MenuDefinition menu = scrollDefinition(m_labels);
    menu.title = std::move(title);
    menu.titleScale = kPageTitleScale;
    menu.body = std::move(passages);
    menu.bodyScale = kPageScale * scale;
    menu.bodyY = centred ? -1 : kPageTop;
    menu.bodyGap = gap;
    menu.backdropFades = false; // the scroll is already out
    m_page.open(menu, painter, m_screen);
}

void HintMenu::close() {
    m_topics = OptionMenu{};
    m_page = OptionMenu{};
    m_fire.reset();
}

/** Backing out of the list hands the scroll to the flames while the words fade. */
void HintMenu::leave(RenderDevice& device) {
    if (m_art.scroll != nullptr && !m_art.burnMasks.empty() && !m_art.burnRing.empty() &&
        m_fire.start(device, m_topics.backdropArea(), *m_art.scroll, m_art.burnMasks,
                     m_art.burnRing)) {
        m_topics.releaseBackdrop();
    }
    m_topics.close();
}

HintMenuEvent HintMenu::update(RenderDevice& device, const MenuInput& input, int ticks) {
    // Nothing answers while the scroll burns, as the original blanks the controls.
    const bool burning = m_fire.active();
    m_fire.step(ticks);
    const MenuInput heard = burning ? MenuInput{} : input;
    if (m_page.isOpen()) {
        const MenuEvent event = m_page.update(heard, ticks);
        if (event.action == MenuAction::Back && m_painter != nullptr) {
            // Back to the list where it was left, the scroll staying as it is.
            const int selection = m_topics.selection();
            m_page = OptionMenu{};
            m_topics.open(topicsDefinition(false), *m_painter, m_screen, selection);
            return HintMenuEvent{HintMenuEvent::Kind::Returned, 0};
        }
        return {};
    }
    if (!m_topics.isOpen()) {
        return {};
    }
    const MenuEvent event = m_topics.update(heard, ticks);
    switch (event.action) {
    case MenuAction::Moved: return HintMenuEvent{HintMenuEvent::Kind::Moved, 0};
    case MenuAction::Choice: return HintMenuEvent{HintMenuEvent::Kind::Asked, event.code};
    case MenuAction::Back: leave(device); return HintMenuEvent{HintMenuEvent::Kind::Left, 0};
    default: return {};
    }
}

void HintMenu::prepare(RenderDevice& device) {
    m_fire.prepare(device);
}

void HintMenu::draw(Canvas& canvas, const TextPainter& painter) const {
    m_fire.draw(canvas);
    if (m_page.isOpen()) {
        m_page.draw(canvas, painter, m_art.textures);
    } else {
        m_topics.draw(canvas, painter, m_art.textures);
    }
}

} // namespace gdl::game
