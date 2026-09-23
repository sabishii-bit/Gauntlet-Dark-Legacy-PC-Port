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
    MenuTextures menuTextures;      ///< sheets the lane menus draw with
    std::int32_t keyboardLane = -1; ///< the lane whose player can also type a name
};

/**
 * One player's column on the select screen: joins on Start, then walks the original's states
 * (the New/Load menu, name entry, class and colour choice, the save menus) until the
 * character is locked in.
 */
class SelectLane {
public:
    static constexpr std::int32_t kWidth = 128;
    static constexpr std::int32_t kPanelHeight = 320;
    static constexpr std::int32_t kNoticeTicks = 120;
    static constexpr std::int32_t kOperationStepTicks = 8;

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
        std::uint32_t slotsInUse = 0; ///< save slots other lanes loaded
    };

    void reset(std::int32_t index, LaneServices* services);

    /** A player joins the lane. */
    void activate();

    Result update(const MenuInput& input, std::int32_t ticks, const Frame& frame);

    /** The lane's pictures: weapon relief, portrait, marks and name plate. */
    void drawImages(Canvas& canvas) const;

    /** The lane's menus, prompts and messages, over the frame. */
    void drawText(Canvas& canvas, std::int32_t time) const;

    bool active() const { return m_state != State::Inactive; }
    bool selecting() const { return active() && m_state != State::LockedIn; }
    bool lockedIn() const { return m_state == State::LockedIn; }
    bool animating() const;
    State state() const { return m_state; }

    /** Whether the lane is taking a name, so typing keys belong to it. */
    bool typing() const { return m_state == State::NameEntry && m_nameEntry.editing(); }
    std::int32_t index() const { return m_index; }
    std::int32_t x() const { return m_index * kWidth; }
    const CharacterSave& save() const { return m_save; }
    bool saved() const { return m_saved; }
    std::int32_t pickedClass() const { return m_pickClass; }
    std::int32_t pickedColor() const { return m_pickColor; }
    std::optional<std::size_t> slotInUse() const { return m_slotInUse; }
    const NameEntry& nameEntry() const { return m_nameEntry; }
    BoxMode boxMode() const;

    /** The class the status box pictures: the one being picked, else the character's. */
    std::int32_t boxClass() const {
        return m_state == State::ClassPick ? m_pickClass : m_save.character;
    }
    std::int32_t boxColor() const {
        return m_state == State::ClassPick ? m_pickColor : m_save.color;
    }

private:
    enum class Sheet : std::uint8_t { Weapon, Portrait, FlyOut, QuestMark, Name };
    enum class Anim : std::uint8_t { None, FadeIn, FadeOut, DelayedFadeOut, FlyOut, Pulse };

    struct Blit {
        std::string texture;
        Anim anim = Anim::None;
        std::int32_t timer = 0;
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
    void changeClass(std::int32_t step, std::int32_t colorStep);
    void setPortrait();
    void play(SelectSound sound) const;
    std::string_view text(std::string_view id) const;
    std::int32_t wrapClass(std::int32_t classIndex, std::int32_t step) const;
    bool classKnown(std::int32_t classIndex) const;
    std::int32_t pickLevel() const;
    void stepAnimations(std::int32_t ticks);
    Blit& blit(Sheet sheet) { return m_blits[static_cast<std::size_t>(sheet)]; }
    const Blit& blit(Sheet sheet) const { return m_blits[static_cast<std::size_t>(sheet)]; }

    void drawBlit(Canvas& canvas, const Blit& blit, Rect area) const;
    void drawLines(Canvas& canvas, const TextPainter& painter, std::int32_t y,
                   std::int32_t lineHeight, float scale, std::string_view lines, Color color) const;
    void drawPrompt(Canvas& canvas, std::string_view icon, std::int32_t y,
                    std::string_view label) const;
    void drawStats(Canvas& canvas, std::int32_t time) const;
    void drawNameEntry(Canvas& canvas, std::int32_t time) const;
    void drawState(Canvas& canvas, std::int32_t time) const;

    LaneServices* m_services = nullptr;
    std::int32_t m_index = 0;
    State m_state = State::Inactive;
    State m_returnState = State::TopMenu;
    std::int32_t m_step = 0;
    std::int32_t m_timer = 0;
    bool m_saved = false;
    bool m_hasCharacter = false; ///< a character was locked in at least once
    bool m_operationFailed = false;
    bool m_promptStart = false; ///< others still choosing: show the Start prompt
    CharacterSave m_save;
    std::int32_t m_pickClass = 0;
    std::int32_t m_pickColor = 0;
    std::optional<std::size_t> m_slotInUse;
    std::optional<std::size_t> m_slotTarget;
    OptionMenu m_menu;
    NameEntry m_nameEntry;
    std::array<Blit, 5> m_blits{};
};

} // namespace gdl::game
