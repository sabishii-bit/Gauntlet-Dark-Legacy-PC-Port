#include "game/tower/HintMenu.h"

#include <utility>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/widgets/DialoguePresentation.h"

namespace gdl::game {

void HintMenu::appendState(NativeStateDigest& digest, const Simulation& state) const {
    const auto& [labels, screen, painter, topics, page, revision] = state;
    digest.string("HintMenu.native.1");
    const auto& [textures, scroll, masks, ring, presentation] = m_art;
    // The font/arrow sheets are draw-only; burn source availability determines
    // whether backing out starts a native timed phase, even before it is lit.
    static_cast<void>(textures);
    static_cast<void>(presentation);
    digest.resource(scroll);
    digest.wide(masks.size());
    for (const auto* mask : masks) {
        digest.resource(mask);
    }
    digest.wide(ring.size());
    for (const auto* frame : ring) {
        digest.resource(frame);
    }
    digest.fields<4>(labels);
    digest.value(screen);
    digest.flag(painter != nullptr);
    if (painter) {
        digest.value(*painter);
    }
    digest.values(topics, page, revision);
}

bool HintMenu::appendNativeDigest(NativeStateDigest& digest) const {
    appendState(digest, m_sim);
    return m_fire.appendNativeDigest(digest);
}

bool HintMenu::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, owner, binding, fire] = saved;
    appendState(digest, state);
    return m_fire.appendLocalDigest(digest, fire);
}

namespace {

constexpr std::string_view kBackdrop = "SCROLL_A";

/** What the list and its pages share: the scroll, the ink, the prompts' row and the tag. */
MenuDefinition scrollDefinition(const HintMenuLabels& labels) {
    MenuDefinition menu;
    menu.y = -1;
    menu.colors.off = HintMenu::kInk;
    menu.prompts = true;
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
    MenuDefinition menu = scrollDefinition(m_sim.labels);
    menu.title = m_sim.labels.title;
    menu.titleScale = kTitleScale;
    menu.x = kTopicsX;
    menu.items = m_sim.labels.topics;
    menu.selectLabel = m_sim.labels.select;
    menu.garamondIntro = true;
    menu.backdropFades = fadeBackdrop;
    return menu;
}

bool HintMenu::open(const TextPainter& painter, const MenuScreen& screen, HintMenuLabels labels,
                    std::optional<MenuDefinition> nativeStyle) {
    close();
    if (!painter.ready() || labels.topics.empty()) {
        return false;
    }
    m_sim.labels = std::move(labels);
    m_sim.screen = screen;
    m_sim.painter = &painter;
    auto definition = nativeStyle ? std::move(*nativeStyle) : topicsDefinition(true);
    if (!nativeStyle) {
        definition.title = m_sim.labels.title;
    }
    definition.items = m_sim.labels.topics;
    m_sim.topics.open(definition, painter, screen);
    ++m_sim.revision;
    return true;
}

void HintMenu::read(const TextPainter& painter, std::string title,
                    std::vector<std::string> passages, f32 scale, bool centred, s32 gap) {
    MenuDefinition menu = scrollDefinition(m_sim.labels);
    menu.title = std::move(title);
    menu.titleScale = kPageTitleScale;
    menu.body = std::move(passages);
    menu.bodyScale = kPageScale * scale;
    menu.bodyY = centred ? -1 : kPageTop;
    menu.bodyGap = gap;
    menu.backdropFades = false; // the scroll is already out
    m_sim.page.open(menu, painter, m_sim.screen);
    ++m_sim.revision;
}

void HintMenu::close() {
    m_sim.topics = OptionMenu{};
    m_sim.page = OptionMenu{};
    m_sim.painter = nullptr;
    m_fire.reset();
    ++m_sim.revision;
}

/** Backing out of the list hands the scroll to the flames while the words fade. */
void HintMenu::leave(RenderDevice& device) {
    if (m_art.scroll != nullptr && !m_art.burnMasks.empty() && !m_art.burnRing.empty() &&
        m_fire.start(device, m_sim.topics.backdropArea(), *m_art.scroll, m_art.burnMasks,
                     m_art.burnRing)) {
        m_sim.topics.releaseBackdrop();
    }
    m_sim.topics.close();
    ++m_sim.revision;
}

bool HintMenu::acceptsTopic(u8 topic, u8 revision) const {
    return topic != 0 && topic <= 4 && revision == m_sim.revision && m_sim.topics.isOpen() &&
           !m_sim.topics.closing() && !m_sim.page.isOpen() && !m_fire.active() &&
           topic <= m_sim.topics.definition().items.size() &&
           m_sim.topics.definition().items[topic - 1].enabled;
}

u8 HintMenu::captureTopic(const MenuInput& input) const {
    const auto choice = m_sim.topics.pointerChoice(input);
    if (!choice || *choice >= 4) {
        return 0;
    }
    const auto topic = static_cast<u8>(*choice + 1);
    return acceptsTopic(topic, m_sim.revision) ? topic : 0;
}

HintMenuEvent HintMenu::update(RenderDevice& device, const MenuInput& input, s32 ticks, u8 topic,
                               u8 revision) {
    // Nothing answers while the scroll burns, as the original blanks the controls.
    const bool burning = m_fire.active();
    m_fire.step(ticks);
    const MenuInput heard = burning ? MenuInput{} : input;
    if (m_sim.page.isOpen()) {
        const MenuEvent event = m_sim.page.update(heard, ticks);
        if (event.action == MenuAction::Back && m_sim.painter != nullptr) {
            // Back to the list where it was left, the scroll staying as it is.
            const s32 selection = m_sim.topics.selection();
            m_sim.page = OptionMenu{};
            m_sim.topics.open(topicsDefinition(false), *m_sim.painter, m_sim.screen, selection);
            ++m_sim.revision;
            return HintMenuEvent{HintMenuEvent::Kind::Returned, 0};
        }
        return {};
    }
    if (!m_sim.topics.isOpen()) {
        return {};
    }
    const auto choice = acceptsTopic(topic, revision)
                            ? std::optional<usize>{static_cast<usize>(topic - 1)}
                            : std::nullopt;
    const MenuEvent event = m_sim.topics.update(heard, ticks, choice);
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
    const auto& text = m_sim.painter ? *m_sim.painter : painter;
    m_fire.draw(canvas);
    if (m_sim.page.isOpen()) {
        m_sim.page.draw(canvas, text, m_art.textures);
    } else {
        auto art = m_art.textures;
        if (m_art.presentation && m_sim.topics.isOpen() && !m_sim.topics.backdropReleased() &&
            m_art.presentation->drawTopicsBackdrop(canvas, m_sim.topics, m_sim.labels.title)) {
            art.backdrop = nullptr;
        }
        m_sim.topics.draw(canvas, text, art);
    }
}

void HintMenu::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    m_fire.saveLocal(saved.m_fire);
}

bool HintMenu::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && saved.m_binding.lock() == m_localBinding &&
           m_fire.canRestoreLocal(saved.m_fire);
}

bool HintMenu::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    m_fire.restoreLocal(saved.m_fire);
    return true;
}

} // namespace gdl::game
