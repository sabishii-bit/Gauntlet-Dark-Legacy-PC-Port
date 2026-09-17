#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/menu/MenuInput.h"
#include "game/menu/NameEntry.h"
#include "game/menu/OptionMenu.h"
#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"

namespace gdl::game {

/** The sounds a lane asks for; the scene maps them to the banks. */
enum class SelectSound : u8 {
    Select,
    ClassChange,
    CursorVertical,
    CursorHorizontal,
    Buzzer,
    LetterAccept,
    Welcome,
    WelcomeBack
};

/** What every lane draws with and reads from; owned by the scene. */
struct LaneServices {
    SaveSlots* slots = nullptr; ///< null when saving is unavailable
    const ClassDataSet* classes = nullptr;
    const StringTable* strings = nullptr;
    const TextPainter* menuPainter = nullptr; ///< lays the menus out
    MenuScreen screen;
    std::function<void(SelectSound)> playSound;
    std::function<const Texture*(std::string_view)> selectTexture; ///< SELECT archive lookup
    std::function<const Texture*(std::string_view)> staticTexture; ///< STATIC archive lookup
    const TextPainter* smallPainter = nullptr;                     ///< the 8x8 font
    const TextPainter* initialsPainter = nullptr;
    const TextPainter* largePainter = nullptr; ///< font32
    const Texture* glowSheet = nullptr;
    MenuTextures menuTextures; ///< sheets the lane menus draw with
};

/**
 * One player's column on the select screen: joins on Start, then walks the original's states
 * (the New/Load menu, name entry, class and colour choice, the save menus) until the
 * character is locked in.
 */
class SelectLane {
public:
    static constexpr s32 kWidth = 128;
    static constexpr s32 kPanelHeight = 320;
    static constexpr s32 kNoticeTicks = 120;
    static constexpr s32 kOperationStepTicks = 8;

    enum class State : u8 {
        Inactive,
        TopMenu,
        SaveMenu,
        QuitConfirm,
        NameEntry,
        ClassPick,
        LoadConfirm,
        LoadPick,
        Loading,
        NoFiles,
        SavePick,
        OverwriteConfirm,
        Saving,
        LockedIn
    };

    enum class Result : u8 { None, Cleared, Leave };

    /** What the status box under the lane shows. */
    enum class BoxMode : u8 {
        Plain,     ///< the tinted stone panel
        Character, ///< the class icon and the name
        Status     ///< the class icon, name and level
    };

    struct Frame {
        bool othersActive = false; ///< another lane holds a player
        bool othersSelecting = false;
        u32 slotsInUse = 0; ///< save slots other lanes loaded
    };

    void reset(s32 index, LaneServices* services);

    /** A player joins the lane. */
    void activate();

    Result update(const MenuInput& input, s32 ticks, const Frame& frame);

    /** The lane's pictures: weapon relief, portrait, marks and name plate. */
    void drawImages(Canvas& canvas) const;

    /** The lane's menus, prompts and messages, over the frame. */
    void drawText(Canvas& canvas, s32 time) const;

    bool active() const { return m_state != State::Inactive; }
    bool selecting() const { return active() && m_state != State::LockedIn; }
    bool lockedIn() const { return m_state == State::LockedIn; }
    bool animating() const;
    State state() const { return m_state; }
    s32 index() const { return m_index; }
    s32 x() const { return m_index * kWidth; }
    const CharacterSave& save() const { return m_save; }
    bool saved() const { return m_saved; }
    s32 pickedClass() const { return m_pickClass; }
    s32 pickedColor() const { return m_pickColor; }
    std::optional<usize> slotInUse() const { return m_slotInUse; }
    const NameEntry& nameEntry() const { return m_nameEntry; }
    BoxMode boxMode() const;

    /** The class the status box pictures: the one being picked, else the character's. */
    s32 boxClass() const { return m_state == State::ClassPick ? m_pickClass : m_save.character; }
    s32 boxColor() const { return m_state == State::ClassPick ? m_pickColor : m_save.color; }

private:
    enum class Sheet : u8 { Weapon, Portrait, FlyOut, QuestMark, Name };
    enum class Anim : u8 { None, FadeIn, FadeOut, DelayedFadeOut, FlyOut, Pulse };

    struct Blit {
        std::string texture;
        Anim anim = Anim::None;
        s32 timer = 0;
        bool visible = false;
        u8 opacity = 255;
    };

    void enter(State state);
    void returnBack();
    void openMenu(State state);
    void openListMenu(State state);
    void openConfirm();
    void lockIn(bool fromLoad);
    void clearPlayer();
    void showClassPick();
    void changeClass(s32 step, s32 colorStep);
    void setPortrait();
    void play(SelectSound sound) const;
    std::string_view text(std::string_view id) const;
    s32 wrapClass(s32 classIndex, s32 step) const;
    bool classKnown(s32 classIndex) const;
    s32 pickLevel() const;
    void stepAnimations(s32 ticks);
    Blit& blit(Sheet sheet) { return m_blits[static_cast<usize>(sheet)]; }
    const Blit& blit(Sheet sheet) const { return m_blits[static_cast<usize>(sheet)]; }

    void drawBlit(Canvas& canvas, const Blit& blit, Rect area) const;
    void drawLines(Canvas& canvas, const TextPainter& painter, s32 y, s32 lineHeight, f32 scale,
                   std::string_view lines, Color color) const;
    void drawPrompt(Canvas& canvas, std::string_view icon, s32 y, std::string_view label) const;
    void drawStats(Canvas& canvas, s32 time) const;
    void drawNameEntry(Canvas& canvas, s32 time) const;
    void drawState(Canvas& canvas, s32 time) const;

    LaneServices* m_services = nullptr;
    s32 m_index = 0;
    State m_state = State::Inactive;
    State m_returnState = State::TopMenu;
    s32 m_step = 0;
    s32 m_timer = 0;
    bool m_saved = false;
    bool m_hasCharacter = false; ///< a character was locked in at least once
    bool m_operationFailed = false;
    bool m_promptStart = false; ///< others still choosing: show the Start prompt
    CharacterSave m_save;
    s32 m_pickClass = 0;
    s32 m_pickColor = 0;
    std::optional<usize> m_slotInUse;
    std::optional<usize> m_slotTarget;
    OptionMenu m_menu;
    NameEntry m_nameEntry;
    std::array<Blit, 5> m_blits{};
};

} // namespace gdl::game
