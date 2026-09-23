#include "game/screens/SelectLane.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>

#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr int kFullOpacity = 255;
constexpr int kMenuCenterY = 128;
constexpr int kListMenuY = 70;
constexpr int kListMenuX = 8;
constexpr int kConfirmMenuY = 164;
constexpr float kMenuScale = 0.667f;
constexpr float kListScale = 0.6f;
constexpr int kMessageY = 100;
constexpr int kLineHeight = 10;
constexpr float kSmallScale = 1.2f;
constexpr float kCaptionScale = 0.8f;
constexpr float kStatScale = 0.5f;
constexpr float kLetterScale = 0.9f;
constexpr float kFlashScale = 0.75f;
constexpr int kPromptX = 20;
constexpr int kPromptSelectY = 252;
constexpr int kPromptBackY = 272;
constexpr int kPromptIconSize = 19;
constexpr int kPromptGap = 8;
constexpr int kPromptTextDrop = 4;
constexpr int kLegendY = 180;
constexpr int kLegendClassY = 232;
constexpr int kLegendStep = 20;
constexpr int kLegendX = 10;
constexpr int kCaptionY = 64;
constexpr int kCaptionStep = 26;
constexpr int kLockedTextY = 142;
constexpr int kLockedLineStep = 12;
constexpr int kStatsY = 162;
constexpr int kStatsStep = 16;
constexpr int kStatNameRight = 81;
constexpr int kStatValueX = 84;
constexpr int kStatGlowInset = 6;
constexpr int kStatGlowWidth = 68;
constexpr int kLevelY = 292;
constexpr int kLettersY = 340;
constexpr int kLetterStep = 18;
/** Where each lane's first name letter starts, a few pixels into the lane. */
constexpr std::array<int, 4> kLetterStartX{8, 10, 10, 7};
constexpr int kNameCenterOffset = 64;
constexpr int kPortraitY = 28;
constexpr int kPortraitHeight = 256;
constexpr int kQuestMarkCenterY = 160;
constexpr int kQuestMarkSize = 64;
constexpr int kNamePlateX = 8;
constexpr int kNamePlateY = 272;
constexpr int kNamePlateWidth = 128;
constexpr int kNamePlateHeight = 16;
constexpr int kFlyOutTicks = 16;
constexpr int kFadeInTicks = 32;
constexpr int kFadeOutDelay = 16;
constexpr int kPulseSpan = 64;
constexpr int kPulseHalf = 32;
constexpr float kPulseStep = 0.025f;
constexpr int kOperationSteps = 3;
constexpr int kGlowTextRadius = 40;
constexpr int kGlowTextHold = 5;
constexpr Color kGlowColor = Color::rgba(130, 0, 234);
constexpr Color kDimLetter = Color::rgba(64, 64, 64);
constexpr int kMenuNew = 1000;
constexpr int kMenuLoad = 1001;
constexpr int kMenuSave = 1002;
constexpr int kMenuChange = 1003;
constexpr int kMenuQuit = 1004;
constexpr int kMenuDone = 1005;
constexpr int kMenuYes = 1006;
constexpr int kMenuNo = 1007;
constexpr int kMenuSlotBase = 2000;
constexpr std::string_view kShadowSuffix = "SHADW";
constexpr std::string_view kSumnerPortrait = "S12_SUM";
constexpr std::string_view kQuestMarkTexture = "SELSCRN_QUESTMARK";
constexpr std::string_view kStatGlowTexture = "ATT_GLOW";
constexpr std::string_view kIconUp = "BUTTON_U";
constexpr std::string_view kIconDown = "BUTTON_D";
constexpr std::string_view kIconLeft = "BUTTON_L";
constexpr std::string_view kIconRight = "BUTTON_R";
constexpr std::string_view kIconSelect = "BUTTON_X";
constexpr std::string_view kIconBack = "BUTTON_TRI";

/** Opacity from the original's blit alpha, where 0 is opaque and 256 invisible. */
std::uint8_t opacityFromBlitAlpha(int alpha) {
    return static_cast<std::uint8_t>(kFullOpacity - std::clamp(alpha, 0, kFullOpacity));
}

} // namespace

void SelectLane::reset(int index, LaneServices* services) {
    m_services = services;
    m_index = index;
    m_state = State::Inactive;
    m_returnState = State::TopMenu;
    m_step = 0;
    m_timer = 0;
    m_saved = false;
    m_hasCharacter = false;
    m_operationFailed = false;
    m_save = CharacterSave{};
    m_pickClass = 0;
    m_pickColor = 0;
    m_slotInUse.reset();
    m_slotTarget.reset();
    m_menu.close();
    m_blits = {};
}

void SelectLane::activate() {
    if (m_state != State::Inactive) {
        return;
    }
    m_save = CharacterSave{};
    m_saved = true; // nothing to lose yet, so no unsaved-character prompts
    m_hasCharacter = false;
    m_slotInUse.reset();
    m_pickClass = 0;
    m_pickColor = 0;
    m_blits = {};
    enter(State::TopMenu);
}

void SelectLane::play(SelectSound sound) const {
    if (m_services != nullptr && m_services->playSound) {
        m_services->playSound(sound, *this);
    }
}

std::string_view SelectLane::text(std::string_view id) const {
    return m_services != nullptr && m_services->strings != nullptr ? m_services->strings->get(id)
                                                                   : id;
}

bool SelectLane::classKnown(int classIndex) const {
    return classUnlocked(classIndex, m_save.classUnlock);
}

int SelectLane::wrapClass(int classIndex, int step) const {
    for (;;) {
        if (classIndex >= kClassCount) {
            classIndex = 0;
        }
        if (classIndex < 0) {
            classIndex = kClassCount - 1;
        }
        if (classIndex == kSumnerClass && !classKnown(kSumnerClass)) {
            classIndex += step == 0 ? 1 : step;
            continue;
        }
        return classIndex;
    }
}

int SelectLane::pickLevel() const {
    if (m_pickClass == kSumnerClass) {
        return kMaxLevel;
    }
    return experienceLevel(m_save.classes[static_cast<std::size_t>(m_pickClass)].experience);
}

void SelectLane::enter(State state) {
    m_state = state;
    m_step = 0;
    m_timer = 0;
    m_menu.close();
    switch (state) {
    case State::TopMenu:
    case State::SaveMenu: openMenu(state); break;
    case State::QuitConfirm:
    case State::LoadConfirm:
    case State::OverwriteConfirm: openConfirm(); break;
    case State::LoadPick:
    case State::SavePick: openListMenu(state); break;
    case State::NameEntry: m_nameEntry.begin(m_save.name); break;
    case State::ClassPick: showClassPick(); break;
    case State::LockedIn: m_returnState = State::SaveMenu; break;
    default: break;
    }
}

void SelectLane::returnBack() {
    enter(m_returnState);
}

void SelectLane::openMenu(State state) {
    MenuDefinition definition;
    definition.x = -(x() + kWidth / 2);
    definition.y = -kMenuCenterY;
    definition.scale = kMenuScale;
    definition.startSelects = false;
    int selection = 0;
    const bool anySaved =
        m_services != nullptr && m_services->slots != nullptr && m_services->slots->anySaved();
    if (state == State::TopMenu) {
        definition.items = {{std::string(text("select.new")), kMenuNew},
                            {std::string(text("select.load")), kMenuLoad}};
        definition.items[1].enabled = anySaved;
        selection = anySaved ? 1 : 0;
    } else {
        definition.items = {{std::string(text("select.save")), kMenuSave},
                            {std::string(text("select.change")), kMenuChange},
                            {std::string(text("select.load")), kMenuLoad},
                            {std::string(text("select.quit")), kMenuQuit},
                            {std::string(text("select.done")), kMenuDone}};
        definition.items[0].enabled = m_services != nullptr && m_services->slots != nullptr;
        definition.items[2].enabled = anySaved;
        selection = 4;
    }
    if (m_services != nullptr && m_services->menuPainter != nullptr) {
        m_menu.open(definition, *m_services->menuPainter, m_services->screen, selection);
    }
}

void SelectLane::openConfirm() {
    MenuDefinition definition;
    definition.x = -(x() + kWidth / 2);
    definition.y = kConfirmMenuY;
    definition.scale = kMenuScale;
    definition.items = {{std::string(text("select.yes")), kMenuYes},
                        {std::string(text("select.no")), kMenuNo}};
    if (m_services != nullptr && m_services->menuPainter != nullptr) {
        m_menu.open(definition, *m_services->menuPainter, m_services->screen, 1);
    }
}

void SelectLane::openListMenu(State state) {
    MenuDefinition definition;
    definition.x = x() + kListMenuX;
    definition.y = kListMenuY;
    definition.scale = kListScale;
    int selection = 0;
    if (m_services != nullptr && m_services->slots != nullptr) {
        const SaveSlots& slots = *m_services->slots;
        for (std::size_t i = 0; i < slots.count(); ++i) {
            const SaveSlotInfo& info = slots.slot(i);
            MenuItem item;
            item.text = info.exists ? info.name : std::string(text("select.empty"));
            item.code = kMenuSlotBase + static_cast<int>(i);
            item.enabled = state == State::SavePick || info.exists;
            definition.items.push_back(item);
            if (m_slotTarget.has_value() && *m_slotTarget == i) {
                selection = static_cast<int>(i);
            }
        }
    }
    if (m_services != nullptr && m_services->menuPainter != nullptr) {
        m_menu.open(definition, *m_services->menuPainter, m_services->screen, selection);
    }
}

void SelectLane::showClassPick() {
    m_pickClass = wrapClass(m_pickClass, 0);
    setPortrait();
    blit(Sheet::Weapon).visible = false;
    blit(Sheet::FlyOut).visible = false;
}

void SelectLane::setPortrait() {
    Blit& portrait = blit(Sheet::Portrait);
    const bool known = classKnown(m_pickClass);
    if (m_pickClass == kSumnerClass) {
        portrait.texture = std::string(kSumnerPortrait);
    } else {
        portrait.texture = std::format("S12_{}_{}", classCode(m_pickClass),
                                       known ? colorCode(m_pickColor) : kShadowSuffix);
    }
    portrait.visible = true;
    portrait.anim = Anim::None;
    portrait.opacity = kFullOpacity;

    Blit& name = blit(Sheet::Name);
    name.texture = std::format("{}_NAME", classCode(m_pickClass));
    name.visible = known;
    name.opacity = kFullOpacity;

    Blit& mark = blit(Sheet::QuestMark);
    mark.texture = std::string(kQuestMarkTexture);
    mark.visible = !known;
    mark.anim = known ? Anim::None : Anim::Pulse;
    mark.opacity = kFullOpacity;
}

void SelectLane::changeClass(int step, int colorStep) {
    Blit& flyOut = blit(Sheet::FlyOut);
    flyOut.texture = blit(Sheet::Portrait).texture;
    flyOut.visible = true;
    flyOut.anim = Anim::FlyOut;
    flyOut.timer = 0;
    flyOut.opacity = kFullOpacity;

    m_pickClass = wrapClass(m_pickClass + step, step);
    m_pickColor = (m_pickColor + colorStep + kColorCount) % kColorCount;
    setPortrait();
    play(SelectSound::ClassChange);
}

/** Takes the picked class as the character and shows it standing ready. */
void SelectLane::lockIn(bool fromLoad) {
    m_save.character = m_pickClass;
    m_save.color = m_pickColor;
    m_hasCharacter = true;
    if (!fromLoad) {
        m_saved = false;
    }
    Blit& weapon = blit(Sheet::Weapon);
    weapon.texture = std::format("S12_WEAP_{}", classCode(m_pickClass & (kStartingClassCount - 1)));
    weapon.visible = true;
    weapon.anim = Anim::FadeIn;
    weapon.timer = 0;
    weapon.opacity = 0;
    Blit& portrait = blit(Sheet::Portrait);
    portrait.anim = Anim::DelayedFadeOut;
    portrait.timer = 0;
    blit(Sheet::QuestMark).visible = false;
    blit(Sheet::FlyOut).visible = false;
    Blit& name = blit(Sheet::Name);
    name.texture = std::format("{}_NAME", classCode(m_pickClass));
    name.visible = true;
    name.opacity = kFullOpacity;
    play(m_save.experience() == 0 ? SelectSound::Welcome : SelectSound::WelcomeBack);
    enter(State::LockedIn);
}

void SelectLane::clearPlayer() {
    m_state = State::Inactive;
    m_menu.close();
    m_blits = {};
    m_slotInUse.reset();
    m_hasCharacter = false;
}

SelectLane::Result SelectLane::update(const MenuInput& input, int ticks, const Frame& frame) {
    stepAnimations(ticks);
    m_promptStart = frame.othersSelecting;
    if (m_state == State::Inactive) {
        return Result::None;
    }
    const MenuEvent event = m_menu.isOpen() ? m_menu.update(input, ticks) : MenuEvent{};
    switch (m_state) {
    case State::TopMenu:
        if (event.action == MenuAction::Back) {
            clearPlayer();
            return frame.othersActive ? Result::Cleared : Result::Leave;
        }
        if (event.action == MenuAction::Choice && event.code == kMenuNew) {
            play(SelectSound::Select);
            m_returnState = State::TopMenu;
            enter(State::NameEntry);
        } else if (event.action == MenuAction::Choice && event.code == kMenuLoad) {
            play(SelectSound::Select);
            m_returnState = State::TopMenu;
            const bool anySaved = m_services != nullptr && m_services->slots != nullptr &&
                                  m_services->slots->anySaved();
            enter(anySaved ? State::LoadPick : State::NoFiles);
        } else if (event.action == MenuAction::Moved) {
            play(SelectSound::CursorVertical);
        }
        break;

    case State::SaveMenu:
        if (event.action == MenuAction::Choice) {
            switch (event.code) {
            case kMenuDone:
                play(SelectSound::Select);
                enter(State::LockedIn);
                break;
            case kMenuSave:
                m_returnState = State::SaveMenu;
                enter(State::SavePick);
                break;
            case kMenuLoad:
                m_returnState = State::SaveMenu;
                enter(m_saved ? State::LoadPick : State::LoadConfirm);
                break;
            case kMenuChange:
                m_returnState = State::SaveMenu;
                m_pickClass = m_save.character;
                m_pickColor = m_save.color;
                enter(State::ClassPick);
                break;
            case kMenuQuit:
                if (m_saved) {
                    clearPlayer();
                    return frame.othersActive ? Result::Cleared : Result::Leave;
                }
                m_returnState = State::SaveMenu;
                enter(State::QuitConfirm);
                break;
            default: break;
            }
        } else if (event.action == MenuAction::Moved) {
            play(SelectSound::CursorVertical);
        }
        break;

    case State::QuitConfirm:
        if (event.action == MenuAction::Choice && event.code == kMenuYes) {
            clearPlayer();
            return frame.othersActive ? Result::Cleared : Result::Leave;
        }
        if (event.action == MenuAction::Back ||
            (event.action == MenuAction::Choice && event.code == kMenuNo)) {
            returnBack();
        } else if (event.action == MenuAction::Moved) {
            play(SelectSound::CursorVertical);
        }
        break;

    case State::NameEntry: {
        // Erasing an empty name does nothing, so a held Backspace cannot leave by accident.
        if ((input.back || input.escape) && m_nameEntry.editing()) {
            returnBack();
            break;
        }
        switch (m_nameEntry.update(input, ticks)) {
        case NameEntry::Event::LetterChanged: play(SelectSound::CursorVertical); break;
        case NameEntry::Event::LetterRemoved: play(SelectSound::CursorHorizontal); break;
        case NameEntry::Event::LetterAdded:
        case NameEntry::Event::Accepted: play(SelectSound::LetterAccept); break;
        default: break;
        }
        if (m_nameEntry.finished()) {
            m_save.name = m_nameEntry.name();
            m_pickClass = wrapClass(m_save.character, 0);
            m_pickColor = m_save.color;
            enter(State::ClassPick);
        }
        break;
    }

    case State::ClassPick:
        if (input.back) {
            blit(Sheet::Portrait).visible = false;
            blit(Sheet::QuestMark).visible = false;
            blit(Sheet::Name).visible = false;
            if (m_returnState == State::TopMenu && !m_hasCharacter) {
                m_save = CharacterSave{};
            }
            returnBack();
            break;
        }
        if (input.left) {
            changeClass(-1, 0);
        } else if (input.right) {
            changeClass(1, 0);
        } else if (input.up) {
            changeClass(0, 1);
        } else if (input.down) {
            changeClass(0, -1);
        }
        if (input.select) {
            if (!classKnown(m_pickClass)) {
                play(SelectSound::Buzzer);
            } else {
                play(SelectSound::Select);
                const bool changing = m_hasCharacter;
                lockIn(false);
                if (changing) {
                    enter(State::SaveMenu);
                }
            }
        }
        break;

    case State::LoadConfirm:
        if (event.action == MenuAction::Choice && event.code == kMenuYes) {
            const bool anySaved = m_services != nullptr && m_services->slots != nullptr &&
                                  m_services->slots->anySaved();
            enter(anySaved ? State::LoadPick : State::NoFiles);
        } else if (event.action == MenuAction::Back ||
                   (event.action == MenuAction::Choice && event.code == kMenuNo)) {
            returnBack();
        } else if (event.action == MenuAction::Moved) {
            play(SelectSound::CursorVertical);
        }
        break;

    case State::NoFiles:
        m_timer += ticks;
        if (m_timer >= kNoticeTicks) {
            returnBack();
        }
        break;

    case State::LoadPick:
    case State::SavePick:
        if (event.action == MenuAction::Back) {
            returnBack();
        } else if (event.action == MenuAction::Choice && event.code >= kMenuSlotBase) {
            const auto slot = static_cast<std::size_t>(event.code - kMenuSlotBase);
            const bool inUse = (frame.slotsInUse & (1U << slot)) != 0;
            if (m_state == State::LoadPick && inUse) {
                play(SelectSound::Buzzer);
                break;
            }
            play(SelectSound::Select);
            m_slotTarget = slot;
            m_operationFailed = false;
            if (m_state == State::LoadPick) {
                enter(State::Loading);
            } else {
                const bool exists = m_services != nullptr && m_services->slots != nullptr &&
                                    m_services->slots->slot(slot).exists;
                enter(exists ? State::OverwriteConfirm : State::Saving);
            }
        } else if (event.action == MenuAction::Moved) {
            play(SelectSound::CursorVertical);
        }
        break;

    case State::OverwriteConfirm:
        if (event.action == MenuAction::Choice && event.code == kMenuYes) {
            enter(State::Saving);
        } else if (event.action == MenuAction::Back ||
                   (event.action == MenuAction::Choice && event.code == kMenuNo)) {
            enter(State::SavePick);
        } else if (event.action == MenuAction::Moved) {
            play(SelectSound::CursorVertical);
        }
        break;

    case State::Loading:
    case State::Saving: {
        // The original steps through the card's asynchronous operations; each step takes a
        // moment here so the progress text reads, then the result stays up a while.
        if (m_step < kOperationSteps) {
            m_timer += ticks;
            while (m_step < kOperationSteps && m_timer >= kOperationStepTicks) {
                m_timer -= kOperationStepTicks;
                ++m_step;
            }
            if (m_step == kOperationSteps) {
                m_timer = 0;
                bool ok = false;
                if (m_services != nullptr && m_services->slots != nullptr &&
                    m_slotTarget.has_value()) {
                    ok = m_state == State::Loading
                             ? m_services->slots->load(*m_slotTarget, m_save)
                             : m_services->slots->write(*m_slotTarget, m_save);
                    if (ok) {
                        m_saved = true;
                        m_slotInUse = m_slotTarget;
                    }
                }
                m_operationFailed = !ok;
            }
            break;
        }
        m_timer += ticks;
        if (m_timer < kNoticeTicks) {
            break;
        }
        if (!m_operationFailed && m_state == State::Loading) {
            m_pickClass = wrapClass(m_save.character, 0);
            m_pickColor = m_save.color;
            enter(State::ClassPick);
        } else {
            returnBack();
        }
        break;
    }

    case State::LockedIn:
        if (input.start) {
            m_returnState = State::SaveMenu;
            enter(State::SaveMenu);
        }
        break;

    case State::Inactive: break;
    }
    return Result::None;
}

SelectLane::BoxMode SelectLane::boxMode() const {
    switch (m_state) {
    case State::ClassPick: return BoxMode::Character;
    case State::SaveMenu:
    case State::QuitConfirm:
    case State::SavePick:
    case State::OverwriteConfirm:
    case State::Saving:
    case State::LockedIn: return BoxMode::Status;
    default: return BoxMode::Plain;
    }
}

bool SelectLane::animating() const {
    return std::ranges::any_of(m_blits, [](const Blit& b) {
        return b.visible && (b.anim == Anim::FadeIn || b.anim == Anim::FadeOut ||
                             b.anim == Anim::DelayedFadeOut || b.anim == Anim::FlyOut);
    });
}

void SelectLane::stepAnimations(int ticks) {
    for (Blit& b : m_blits) {
        if (!b.visible) {
            continue;
        }
        switch (b.anim) {
        case Anim::FadeIn: {
            b.timer += ticks;
            const int half = b.timer / 2;
            const int alpha = std::min(half * half, kFullOpacity + 1);
            b.opacity = opacityFromBlitAlpha(kFullOpacity + 1 - alpha);
            if (b.timer >= kFadeInTicks) {
                b.opacity = kFullOpacity;
                b.anim = Anim::None;
            }
            break;
        }
        case Anim::FadeOut:
        case Anim::DelayedFadeOut: {
            b.timer += ticks;
            int half = b.timer / 2;
            if (b.anim == Anim::DelayedFadeOut) {
                half = std::max(0, half - kFadeOutDelay);
            }
            const int alpha = half * half;
            b.opacity = opacityFromBlitAlpha(alpha);
            if (alpha >= kFullOpacity + 1) {
                b.visible = false;
                b.anim = Anim::None;
            }
            break;
        }
        case Anim::FlyOut: {
            b.timer += ticks;
            const int u = b.timer * b.timer;
            b.opacity = opacityFromBlitAlpha(u);
            if (b.timer >= kFlyOutTicks) {
                b.visible = false;
                b.anim = Anim::None;
            }
            break;
        }
        case Anim::Pulse: b.timer = (b.timer + ticks) % kPulseSpan; break;
        case Anim::None: break;
        }
    }
}

void SelectLane::drawBlit(Canvas& canvas, const Blit& b, Rect area) const {
    if (!b.visible || m_services == nullptr || !m_services->selectTexture) {
        return;
    }
    const Texture* texture = m_services->selectTexture(b.texture);
    if (texture == nullptr) {
        return;
    }
    canvas.draw(*texture, area, Color::white().withAlpha(b.opacity));
}

void SelectLane::drawLines(Canvas& canvas, const TextPainter& painter, int y, int lineHeight,
                           float scale, std::string_view lines, Color color) const {
    TextStyle style;
    style.scale = scale;
    style.color = color;
    const int centerX = -(x() + kNameCenterOffset);
    std::size_t start = 0;
    while (start <= lines.size()) {
        const std::size_t end = lines.find('\n', start);
        const std::string_view line = lines.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        painter.draw(canvas, centerX, y, line, style);
        y += lineHeight;
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
}

void SelectLane::drawPrompt(Canvas& canvas, std::string_view icon, int y,
                            std::string_view label) const {
    if (m_services == nullptr || m_services->smallPainter == nullptr) {
        return;
    }
    const int iconX = x() + kPromptX;
    if (m_services->staticTexture) {
        if (const Texture* texture = m_services->staticTexture(icon)) {
            canvas.draw(*texture, Rect{static_cast<float>(iconX), static_cast<float>(y),
                                       static_cast<float>(kPromptIconSize),
                                       static_cast<float>(kPromptIconSize)});
        }
    }
    TextStyle style;
    style.scale = kSmallScale;
    m_services->smallPainter->draw(canvas, iconX + kPromptIconSize + kPromptGap,
                                   y + kPromptTextDrop, label, style);
}

void SelectLane::drawStats(Canvas& canvas, int time) const {
    if (m_services == nullptr || m_services->largePainter == nullptr ||
        m_services->initialsPainter == nullptr || m_services->smallPainter == nullptr) {
        return;
    }
    const bool known = classKnown(m_pickClass);
    if (!known) {
        return;
    }
    const int level = pickLevel();
    StatBlock stats;
    bool haveStats = false;
    if (m_pickClass == kSumnerClass) {
        stats = masteryStats();
        haveStats = true;
    } else if (m_services->classes != nullptr) {
        if (const ClassStats* classStats = m_services->classes->stats(m_pickClass)) {
            stats = displayStats(*classStats, level,
                                 m_save.classes[static_cast<std::size_t>(m_pickClass)]);
            haveStats = true;
        }
    }
    if (haveStats) {
        const std::array<std::string_view, StatBlock::kCount> ids{"select.strength", "select.speed",
                                                                  "select.armor", "select.magic"};
        const std::size_t best = m_pickClass == kSumnerClass ? StatBlock::kCount : stats.best();
        int y = kStatsY;
        for (std::size_t row = 0; row < StatBlock::kCount; ++row, y += kStatsStep) {
            const std::string_view name = text(ids[row]);
            const int width = m_services->largePainter->measure(name, kStatScale);
            const int nameX = x() + kStatNameRight - width;
            TextStyle style;
            style.scale = kStatScale;
            if (row == best) {
                TextStyle glow = style;
                glow.color =
                    kGlowColor.withAlpha(pulseOpacity(time, kGlowTextRadius, kGlowTextHold));
                glow.texture = m_services->glowSheet;
                glow.expand = OptionMenu::kGlowExpand;
                m_services->largePainter->draw(canvas, nameX, y - 2, name, glow);
            }
            m_services->largePainter->draw(canvas, nameX, y - 2, name, style);

            const int valueX = x() + kStatValueX;
            if (row == best && m_services->selectTexture) {
                if (const Texture* glowTexture = m_services->selectTexture(kStatGlowTexture)) {
                    const float height = static_cast<float>(kStatGlowWidth) *
                                         static_cast<float>(glowTexture->height()) /
                                         static_cast<float>(glowTexture->width());
                    canvas.draw(*glowTexture, Rect{static_cast<float>(valueX - kStatGlowInset),
                                                   static_cast<float>(y - kStatGlowInset),
                                                   static_cast<float>(kStatGlowWidth), height});
                }
            }
            m_services->initialsPainter->draw(canvas, valueX, y,
                                              std::format("{:03}", stats.values[row]), style);
        }
    }
    const bool hasExperience = m_pickClass != kSumnerClass &&
                               m_save.classes[static_cast<std::size_t>(m_pickClass)].experience > 0;
    const std::string levelText =
        hasExperience ? std::vformat(text("select.level"), std::make_format_args(level))
                      : std::string(text("select.newLevel"));
    drawLines(canvas, *m_services->smallPainter, kLevelY, kLineHeight, kSmallScale, levelText,
              Color::white());
}

void SelectLane::drawNameEntry(Canvas& canvas, int time) const {
    if (m_services == nullptr || m_services->initialsPainter == nullptr) {
        return;
    }
    const TextPainter& painter = *m_services->initialsPainter;
    const Color tint = playerColor(m_index);
    if (m_nameEntry.editing()) {
        TextStyle style;
        style.scale = kLetterScale;
        int letterX = x() + kLetterStartX[static_cast<std::size_t>(m_index) % kLetterStartX.size()];
        std::size_t drawn = 0;
        for (const char letter : m_nameEntry.name()) {
            style.color = tint;
            painter.draw(canvas, letterX, kLettersY, std::string_view(&letter, 1), style);
            letterX += kLetterStep;
            ++drawn;
        }
        if (drawn < NameEntry::kMaxLength) {
            const char pending = m_nameEntry.pendingLetter();
            style.color = (time & NameEntry::kFlashPeriod) != 0 ? Color::white() : kDimLetter;
            painter.draw(canvas, letterX, kLettersY, std::string_view(&pending, 1), style);
            letterX += kLetterStep;
            ++drawn;
        }
        style.color = Color::white();
        for (; drawn < NameEntry::kMaxLength; ++drawn, letterX += kLetterStep) {
            painter.draw(canvas, letterX, kLettersY, "_", style);
        }
    } else if (m_nameEntry.flashing() && m_nameEntry.flashVisible()) {
        TextStyle style;
        style.scale = kFlashScale;
        style.color = tint;
        painter.draw(canvas, -(x() + kNameCenterOffset), kLettersY, m_nameEntry.name(), style);
    }
}

void SelectLane::drawState(Canvas& canvas, int time) const {
    if (m_services == nullptr || m_services->smallPainter == nullptr ||
        m_services->largePainter == nullptr) {
        return;
    }
    const TextPainter& small = *m_services->smallPainter;
    bool showSelect = true;
    bool showBack = false;
    switch (m_state) {
    case State::TopMenu: showBack = true; break;
    case State::SaveMenu: break;
    case State::NameEntry: {
        showSelect = false;
        drawLines(canvas, *m_services->largePainter, kCaptionY, kCaptionStep, kCaptionScale,
                  text("select.enterName"), Color::white());
        if (m_services->staticTexture) {
            const int leftX = x() + kLegendX;
            const int rightX = leftX + kPromptIconSize;
            const auto icon = [&](std::string_view name, int iconX, int iconY) {
                if (const Texture* texture = m_services->staticTexture(name)) {
                    canvas.draw(*texture, Rect{static_cast<float>(iconX), static_cast<float>(iconY),
                                               static_cast<float>(kPromptIconSize),
                                               static_cast<float>(kPromptIconSize)});
                }
            };
            TextStyle style;
            style.scale = kSmallScale;
            const int labelX = rightX + kPromptIconSize + kPromptGap;
            icon(kIconUp, leftX, kLegendY);
            icon(kIconDown, rightX, kLegendY);
            small.draw(canvas, labelX, kLegendY + kPromptTextDrop, text("select.changeLetter"),
                       style);
            icon(kIconLeft, leftX, kLegendY + kLegendStep);
            icon(kIconRight, rightX, kLegendY + kLegendStep);
            small.draw(canvas, labelX, kLegendY + kLegendStep + kPromptTextDrop,
                       text("select.editLetter"), style);
            icon(kIconSelect, rightX, kLegendY + kLegendStep * 2);
            small.draw(canvas, labelX, kLegendY + kLegendStep * 2 + kPromptTextDrop,
                       text("select.accept"), style);
            icon(kIconBack, rightX, kLegendY + kLegendStep * 3);
            small.draw(canvas, labelX, kLegendY + kLegendStep * 3 + kPromptTextDrop,
                       text("select.cancel"), style);
            if (m_services->keyboardLane == m_index) {
                drawLines(canvas, small, kLegendY + kLegendStep * 4 + kPromptTextDrop, kLineHeight,
                          kSmallScale, text("select.typeName"), Color::white());
            }
        }
        drawNameEntry(canvas, time);
        break;
    }
    case State::ClassPick: {
        showSelect = classKnown(m_pickClass);
        if (m_services->staticTexture) {
            const int leftX = x() + kLegendX;
            const int rightX = leftX + kPromptIconSize;
            for (const auto& [name, iconX] :
                 {std::pair{kIconLeft, leftX}, std::pair{kIconRight, rightX}}) {
                if (const Texture* texture = m_services->staticTexture(name)) {
                    canvas.draw(*texture,
                                Rect{static_cast<float>(iconX), static_cast<float>(kLegendClassY),
                                     static_cast<float>(kPromptIconSize),
                                     static_cast<float>(kPromptIconSize)});
                }
            }
            TextStyle style;
            style.scale = kSmallScale;
            small.draw(canvas, rightX + kPromptIconSize + kPromptGap,
                       kLegendClassY + kPromptTextDrop, text("select.changeLetter"), style);
        }
        drawStats(canvas, time);
        break;
    }
    case State::LoadConfirm:
        drawLines(canvas, small, kConfirmMenuY - kLineHeight * 6 - 8, kLineHeight, kSmallScale,
                  text("select.unsavedLoad"), Color::white());
        break;
    case State::QuitConfirm:
        drawLines(canvas, small, kMessageY, kLineHeight, kSmallScale, text("select.quitConfirm"),
                  Color::white());
        break;
    case State::NoFiles:
        drawLines(canvas, small, kMessageY, kLineHeight, kSmallScale, text("select.noFiles"),
                  Color::white());
        break;
    case State::LoadPick:
        showBack = true;
        drawLines(canvas, small, kListMenuY - kLineHeight * 2 - 8, kLineHeight, kSmallScale,
                  text("select.chooseLoad"), Color::white());
        break;
    case State::SavePick:
        showBack = true;
        drawLines(canvas, small, kListMenuY - kLineHeight * 3 - 8, kLineHeight, kSmallScale,
                  text("select.chooseSave"), Color::white());
        break;
    case State::OverwriteConfirm:
        drawLines(canvas, small, kMessageY, kLineHeight, kSmallScale, text("select.overwrite"),
                  Color::white());
        break;
    case State::Loading:
    case State::Saving: {
        showSelect = false;
        const bool saving = m_state == State::Saving;
        std::string_view id;
        if (m_step < kOperationSteps) {
            id = saving ? "select.saving" : "select.loadingFile";
        } else if (m_operationFailed) {
            id = saving ? "select.saveFailed" : "select.loadFailed";
        } else {
            id = saving ? "select.saveComplete" : "select.loadComplete";
        }
        drawLines(canvas, small, kMessageY, kLineHeight, kSmallScale, text(id), Color::white());
        break;
    }
    case State::LockedIn:
        showSelect = false;
        if (m_promptStart && !animating()) {
            drawLines(canvas, small, kLockedTextY, kLockedLineStep, kSmallScale,
                      text("select.lockedIn"), Color::white());
        }
        break;
    case State::Inactive: showSelect = false; break;
    }
    if (showSelect) {
        drawPrompt(canvas, kIconSelect, kPromptSelectY, text("select.select"));
    }
    if (showBack) {
        drawPrompt(canvas, kIconBack, kPromptBackY, text("select.back"));
    }
    if (m_state == State::SaveMenu) {
        const int level = experienceLevel(m_save.experience());
        drawLines(canvas, small, kLevelY, kLineHeight, kSmallScale,
                  std::vformat(text("select.level"), std::make_format_args(level)), Color::white());
    }
}

void SelectLane::drawImages(Canvas& canvas) const {
    const auto left = static_cast<float>(x());
    drawBlit(canvas, blit(Sheet::Weapon),
             Rect{left, 0.0f, static_cast<float>(kWidth), static_cast<float>(kPanelHeight)});
    drawBlit(canvas, blit(Sheet::Portrait),
             Rect{left, static_cast<float>(kPortraitY), static_cast<float>(kWidth),
                  static_cast<float>(kPortraitHeight)});
    const Blit& flyOut = blit(Sheet::FlyOut);
    if (flyOut.visible) {
        const int t = flyOut.timer;
        const int u = t * t;
        const int drift = u / 8;
        drawBlit(canvas, flyOut,
                 Rect{left + static_cast<float>(drift), static_cast<float>(kPortraitY + u + t * 3),
                      static_cast<float>(kWidth), static_cast<float>(kPortraitHeight - u)});
    }
    const Blit& mark = blit(Sheet::QuestMark);
    if (mark.visible) {
        int amplitude = mark.timer;
        if (amplitude >= kPulseHalf) {
            amplitude = kPulseHalf - (amplitude & (kPulseHalf - 1));
        }
        const float size = static_cast<float>(kQuestMarkSize) *
                           (1.0f + kPulseStep * static_cast<float>(amplitude));
        drawBlit(canvas, mark,
                 Rect{left + static_cast<float>(kWidth) / 2.0f - size / 2.0f,
                      static_cast<float>(kQuestMarkCenterY) - size / 2.0f, size, size});
    }
    drawBlit(canvas, blit(Sheet::Name),
             Rect{left + static_cast<float>(kNamePlateX), static_cast<float>(kNamePlateY),
                  static_cast<float>(kNamePlateWidth), static_cast<float>(kNamePlateHeight)});
}

void SelectLane::drawText(Canvas& canvas, int time) const {
    if (m_menu.isOpen() && m_services != nullptr && m_services->menuPainter != nullptr) {
        m_menu.draw(canvas, *m_services->menuPainter, m_services->menuTextures);
    }
    drawState(canvas, time);
}

} // namespace gdl::game
