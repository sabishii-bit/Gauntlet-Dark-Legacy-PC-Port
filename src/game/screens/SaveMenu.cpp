#include "game/screens/SaveMenu.h"

#include <algorithm>
#include <format>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr usize kPageSize = 6;
constexpr s32 kPrevious = -2;
constexpr s32 kNext = -3;
constexpr s32 kBack = -1;
} // namespace
std::string SaveMenu::text(std::string_view id) const {
    return std::string(m_strings != nullptr ? m_strings->get(id) : id);
}
void SaveMenu::open(SaveSlots& slots, std::span<const PartyMember> party, s32 player, Mode mode,
                    const StringTable* strings) {
    m_slots = &slots;
    m_strings = strings;
    m_party.assign(party.begin(), party.end());
    m_mode = mode;
    m_state = State::Done;
    m_success = false;
    m_page = 0;
    m_player = m_party.size();
    slots.refresh();
    for (usize i = 0; i < m_party.size(); ++i) {
        if (m_party[i].player == player) {
            m_player = i;
            m_state = State::Slots;
            break;
        }
    }
}
bool SaveMenu::available(usize slot) const {
    if (m_slots == nullptr || slot >= m_slots->count() || m_player >= m_party.size()) {
        return false;
    }
    for (usize i = 0; i < m_party.size(); ++i) {
        if (i != m_player && m_party[i].slot == slot) {
            return false;
        }
    }
    return m_mode == Mode::Save || m_slots->slot(slot).exists;
}
MenuDefinition SaveMenu::definition() const {
    MenuDefinition menu;
    menu.title = text(m_mode == Mode::Save ? "select.save" : "select.load");
    menu.scale = 0.667f;
    if (m_state == State::Confirm) {
        menu.title = text(m_mode == Mode::Save ? "files.overwrite" : "files.loadConfirm");
        if (m_mode == Mode::Load) {
            menu.body = {text("files.loadWarning")};
        }
        menu.items = {{text("files.no"), 0}, {text("files.yes"), 1}};
    } else if (m_state == State::Notice) {
        if (m_success) {
            menu.title = text(m_mode == Mode::Save ? "files.saved" : "files.loaded");
        } else {
            menu.title = text(m_mode == Mode::Save ? "select.saveFailed" : "select.loadFailed");
        }
        menu.items = {{text("files.ok"), 0}};
    } else if (m_state == State::Slots) {
        if (m_slots == nullptr || m_slots->count() == 0) {
            menu.title = text("files.unavailable");
        }
        if (m_slots != nullptr) {
            for (usize i = m_page * kPageSize;
                 i < std::min(m_slots->count(), (m_page + 1) * kPageSize); ++i) {
                const auto& slot = m_slots->slot(i);
                const auto label = slot.exists
                                       ? slot.name
                                       : text(slot.occupied ? "files.unreadable" : "files.empty");
                menu.items.push_back({std::format("{} {}: {}", text("files.slot"), i + 1, label),
                                      static_cast<s32>(i), 0, available(i)});
            }
            if (m_page > 0) {
                menu.items.push_back({text("files.previous"), kPrevious});
            }
            if ((m_page + 1) * kPageSize < m_slots->count()) {
                menu.items.push_back({text("files.next"), kNext});
            }
        }
        menu.items.push_back({text("menu.back"), kBack});
    }
    return menu;
}
void SaveMenu::choose(s32 code) {
    if (m_state == State::Notice) {
        m_state = m_success ? State::Done : State::Slots;
        return;
    }
    if (m_state == State::Confirm) {
        if (code == 1) {
            perform();
        } else {
            m_state = State::Slots;
        }
        return;
    }
    if (m_state != State::Slots) {
        return;
    }
    if (code == kBack) {
        m_state = State::Done;
    } else if (code == kPrevious && m_page > 0) {
        --m_page;
    } else if (code == kNext && m_slots != nullptr && (m_page + 1) * kPageSize < m_slots->count()) {
        ++m_page;
    } else if (code >= 0 && available(static_cast<usize>(code))) {
        m_target = static_cast<usize>(code);
        m_slots->refresh();
        if (!available(m_target)) {
            return;
        }
        if (m_mode == Mode::Load || m_slots->slot(m_target).occupied) {
            m_state = State::Confirm;
        } else {
            perform();
        }
    }
}
void SaveMenu::perform() {
    if (!available(m_target)) {
        m_success = false;
        m_state = State::Notice;
        return;
    }
    auto& member = m_party[m_player];
    if (m_mode == Mode::Save) {
        m_success = m_slots->write(m_target, member.save);
    } else {
        CharacterSave loaded;
        m_success = m_slots->load(m_target, loaded);
        if (m_success) {
            member.save = std::move(loaded);
            member.fallen = false;
            member.turbo = 0;
            member.helpHeard.clear();
        }
    }
    if (m_success) {
        member.slot = m_target;
    }
    m_state = State::Notice;
}
void SaveMenu::back() {
    if (m_state == State::Confirm || (m_state == State::Notice && !m_success)) {
        m_state = State::Slots;
    } else {
        m_state = State::Done;
    }
}
} // namespace gdl::game
