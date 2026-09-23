#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "engine/assets/StringTable.h"
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
enum class SelectSound : std::uint8_t {
    Select,
    ClassChange,
    CursorVertical,
    CursorHorizontal,
    Buzzer,
    LetterAccept,
    Welcome,
    WelcomeBack
};

class SelectLane;

/** What every lane draws with and reads from; owned by the scene. */
struct LaneServices {
    SaveSlots* slots = nullptr; ///< null when saving is unavailable
    const ClassDataSet* classes = nullptr;
    const StringTable* strings = nullptr;
    const TextPainter* menuPainter = nullptr; ///< lays the menus out
    MenuScreen screen;
    std::function<void(SelectSound, const SelectLane&)> playSound; ///< the lane, for greetings
    std::function<const Texture*(std::string_view)> selectTexture; ///< SELECT archive lookup
    std::function<const Texture*(std::string_view)> staticTexture; ///< STATIC archive lookup
    const TextPainter* smallPainter = nullptr;                     ///< the 8x8 font
    const TextPainter* initialsPainter = nullptr;
    const TextPainter* largePainter = nullptr; ///< font32
    const Texture* glowSheet = nullptr;
    MenuTextures menuTextures; ///< sheets the lane menus draw with
    int keyboardLane = -1;     ///< the lane whose player can also type a name
};

/**
 * One player's column on the select screen: joins on Start, then walks the original's states
 * (the New/Load menu, name entry, class and colour choice, the save menus) until the
 * character is locked in.
 */
class SelectLane {
public:
    static constexpr int kWidth = 128;
    static constexpr int kPanelHeight = 320;
    static constexpr int kNoticeTicks = 120;
    static constexpr int kOperationStepTicks = 8;

    enum class State : std::uint8_t {
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

    enum class Result : std::uint8_t { None, Cleared, Leave };

    /** What the status box under the lane shows. */
    enum class BoxMode : std::uint8_t {
        Plain,     ///< the tinted stone panel
        Character, ///< the class icon and the name
        Status     ///< the class icon, name and level
    };

    struct Frame {
        bool othersActive = false; ///< another lane holds a player
        bool othersSelecting = false;
        unsigned int slotsInUse = 0; ///< save slots other lanes loaded
    };

    void reset(int index, LaneServices* services);

    /** A player joins the lane. */
    void activate();

    Result update(const MenuInput& input, int ticks, const Frame& frame);

    /** The lane's pictures: weapon relief, portrait, marks and name plate. */
    void drawImages(Canvas& canvas) const;

    /** The lane's menus, prompts and messages, over the frame. */
    void drawText(Canvas& canvas, int time) const;

    bool active() const { return m_state != State::Inactive; }
    bool selecting() const { return active() && m_state != State::LockedIn; }
    bool lockedIn() const { return m_state == State::LockedIn; }
    bool animating() const;
    State state() const { return m_state; }

    /** Whether the lane is taking a name, so typing keys belong to it. */
    bool typing() const { return m_state == State::NameEntry && m_nameEntry.editing(); }
    int index() const { return m_index; }
    int x() const { return m_index * kWidth; }
    const CharacterSave& save() const { return m_save; }
    bool saved() const { return m_saved; }
    int pickedClass() const { return m_pickClass; }
    int pickedColor() const { return m_pickColor; }
    std::optional<std::size_t> slotInUse() const { return m_slotInUse; }
    const NameEntry& nameEntry() const { return m_nameEntry; }
    BoxMode boxMode() const;

    /** The class the status box pictures: the one being picked, else the character's. */
    int boxClass() const { return m_state == State::ClassPick ? m_pickClass : m_save.character; }
    int boxColor() const { return m_state == State::ClassPick ? m_pickColor : m_save.color; }

private:
    enum class Sheet : std::uint8_t { Weapon, Portrait, FlyOut, QuestMark, Name };
    enum class Anim : std::uint8_t { None, FadeIn, FadeOut, DelayedFadeOut, FlyOut, Pulse };

    struct Blit {
        std::string texture;
        Anim anim = Anim::None;
        int timer = 0;
        bool visible = false;
        std::uint8_t opacity = 255;
    };

    void enter(State state);
    void returnBack();
    void openMenu(State state);
    void openListMenu(State state);
    void openConfirm();
    void lockIn(bool fromLoad);
    void clearPlayer();
    void showClassPick();
    void changeClass(int step, int colorStep);
    void setPortrait();
    void play(SelectSound sound) const;
    std::string_view text(std::string_view id) const;
    int wrapClass(int classIndex, int step) const;
    bool classKnown(int classIndex) const;
    int pickLevel() const;
    void stepAnimations(int ticks);
    Blit& blit(Sheet sheet) { return m_blits[static_cast<std::size_t>(sheet)]; }
    const Blit& blit(Sheet sheet) const { return m_blits[static_cast<std::size_t>(sheet)]; }

    void drawBlit(Canvas& canvas, const Blit& blit, Rect area) const;
    void drawLines(Canvas& canvas, const TextPainter& painter, int y, int lineHeight, float scale,
                   std::string_view lines, Color color) const;
    void drawPrompt(Canvas& canvas, std::string_view icon, int y, std::string_view label) const;
    void drawStats(Canvas& canvas, int time) const;
    void drawNameEntry(Canvas& canvas, int time) const;
    void drawState(Canvas& canvas, int time) const;

    LaneServices* m_services = nullptr;
    int m_index = 0;
    State m_state = State::Inactive;
    State m_returnState = State::TopMenu;
    int m_step = 0;
    int m_timer = 0;
    bool m_saved = false;
    bool m_hasCharacter = false; ///< a character was locked in at least once
    bool m_operationFailed = false;
    bool m_promptStart = false; ///< others still choosing: show the Start prompt
    CharacterSave m_save;
    int m_pickClass = 0;
    int m_pickColor = 0;
    std::optional<std::size_t> m_slotInUse;
    std::optional<std::size_t> m_slotTarget;
    OptionMenu m_menu;
    NameEntry m_nameEntry;
    std::array<Blit, 5> m_blits{};
};

} // namespace gdl::game
