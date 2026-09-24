#pragma once

#include <span>
#include <vector>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "game/menu/OptionMenu.h"
#include "game/players/Party.h"

namespace gdl::game {
/** Confirmed character-file operations for one player in a paused party. */
class SaveMenu {
public:
    enum class Mode : u8 { Save, Load };
    enum class State : u8 { Slots, Confirm, Notice, Done };
    void open(SaveSlots& slots, std::span<const PartyMember> party, s32 player, Mode mode,
              const StringTable* strings);
    MenuDefinition definition() const;
    void choose(s32 code);
    void back();
    State state() const { return m_state; }
    bool succeeded() const { return m_success; }
    Mode mode() const { return m_mode; }
    const std::vector<PartyMember>& party() const { return m_party; }
    bool available(usize slot) const;

private:
    void perform();
    std::string text(std::string_view id) const;
    SaveSlots* m_slots = nullptr;
    const StringTable* m_strings = nullptr;
    std::vector<PartyMember> m_party;
    usize m_player = 0;
    usize m_page = 0;
    usize m_target = 0;
    Mode m_mode = Mode::Save;
    State m_state = State::Done;
    bool m_success = false;
};
} // namespace gdl::game
