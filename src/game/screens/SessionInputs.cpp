#include "game/screens/SessionInputs.h"

#include "engine/core/Assert.h"

namespace gdl::game {
namespace {
template <typename Button, typename InputState> struct Binding {
    Button button;
    bool InputState::*member;
};

constexpr std::array kPlayHeld{
    Binding{CommandHeld::Attack, &PlayInput::attack},
    Binding{CommandHeld::ShieldPotion, &PlayInput::shieldPotion},
    Binding{CommandHeld::Strafe, &PlayInput::strafe},
    Binding{CommandHeld::StrongAttack, &PlayInput::strongAttack},
    Binding{CommandHeld::Turbo, &PlayInput::turbo},
    Binding{CommandHeld::Combo, &PlayInput::combo},
};
constexpr std::array kPlayPress{
    Binding{CommandPress::UsePotion, &PlayInput::usePotion},
    Binding{CommandPress::ThrowPotion, &PlayInput::throwPotion},
    Binding{CommandPress::Defend, &PlayInput::defendPressed},
    Binding{CommandPress::Charge, &PlayInput::chargePressed},
    Binding{CommandPress::Attack, &PlayInput::attackPressed},
    Binding{CommandPress::TurboAttack, &PlayInput::turboAttackPressed},
};
constexpr std::array kSelectorPress{
    Binding{CommandPress::SelectorUp, &SelectorInput::up},
    Binding{CommandPress::SelectorDown, &SelectorInput::down},
    Binding{CommandPress::SelectorLeft, &SelectorInput::left},
    Binding{CommandPress::SelectorRight, &SelectorInput::right},
};
constexpr std::array kMenuHeld{
    Binding{CommandHeld::MenuUp, &MenuInput::upHeld},
    Binding{CommandHeld::MenuDown, &MenuInput::downHeld},
    Binding{CommandHeld::MenuLeft, &MenuInput::leftHeld},
    Binding{CommandHeld::MenuRight, &MenuInput::rightHeld},
};
constexpr std::array kMenuPress{
    Binding{CommandPress::MenuUp, &MenuInput::up},
    Binding{CommandPress::MenuDown, &MenuInput::down},
    Binding{CommandPress::MenuLeft, &MenuInput::left},
    Binding{CommandPress::MenuRight, &MenuInput::right},
    Binding{CommandPress::MenuSelect, &MenuInput::select},
    Binding{CommandPress::MenuBack, &MenuInput::back},
    Binding{CommandPress::MenuStart, &MenuInput::start},
    Binding{CommandPress::MenuEscape, &MenuInput::escape},
    Binding{CommandPress::MenuAny, &MenuInput::buttonPressed},
};

template <typename Button, typename InputState, usize Size>
u32 pack(const InputState& input, const std::array<Binding<Button, InputState>, Size>& bindings) {
    u32 bits = 0;
    for (const auto& binding : bindings) {
        if (input.*binding.member) {
            bits |= static_cast<u32>(binding.button);
        }
    }
    return bits;
}

template <typename Button, typename InputState, usize Size>
void unpack(InputState& input, u32 bits,
            const std::array<Binding<Button, InputState>, Size>& bindings) {
    for (const auto& binding : bindings) {
        input.*binding.member = (bits & static_cast<u32>(binding.button)) != 0;
    }
}
} // namespace

SessionInputs::SessionInputs() {
    for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
        m_timeline.assign(seat, kLocalPeer);
    }
}

SessionInputs::Frame SessionInputs::advance(const Frame& local) {
    for (usize seat = 0; seat < local.size(); ++seat) {
        if (m_timeline.owner(seat) == kLocalPeer) {
            const auto admission = m_timeline.submit(
                kLocalPeer, command(local[seat], m_timeline.epoch(), m_timeline.tick(),
                                    m_timeline.grant(seat), static_cast<u8>(seat)));
            GDL_VERIFY(admission == InputTimeline::Admission::Accepted,
                       "Local controls must be valid and submitted exactly once per tick");
        }
    }
    const auto commands = m_timeline.advance();
    Frame result;
    for (usize seat = 0; seat < result.size(); ++seat) {
        result[seat] = playInput(commands[seat]);
        if (m_timeline.owner(seat) == kLocalPeer) {
            result[seat].menu = local[seat].menu;
        }
    }
    return result;
}

InputCommand SessionInputs::command(const PlayInput& input, u64 epoch, u64 tick, u32 grant,
                                    u8 seat) {
    InputCommand result;
    result.epoch = epoch;
    result.tick = tick;
    result.grant = grant;
    result.seat = seat;
    result.direction = input.move.direction;
    result.magnitude = input.move.magnitude;
    result.aimPoint = input.aimPoint;
    result.heldButtons = pack(input, kPlayHeld) | pack(input.menu, kMenuHeld);
    result.pressedButtons = pack(input, kPlayPress) | pack(input.selector, kSelectorPress) |
                            pack(input.menu, kMenuPress);
    // Scroll dismissal accepts any action, including a mouse click. Pointer positions
    // and text remain local; remote menu selection needs semantic transactions later.
    if (input.menu.pointerPressed || input.menu.pointerBack) {
        result.pressedButtons |= static_cast<u32>(CommandPress::MenuAny);
    }
    return result;
}

PlayInput SessionInputs::playInput(const InputCommand& command) {
    PlayInput result;
    result.move = {command.direction, command.magnitude};
    result.aimPoint = command.aimPoint;
    unpack(result, command.heldButtons, kPlayHeld);
    unpack(result, command.pressedButtons, kPlayPress);
    unpack(result.selector, command.pressedButtons, kSelectorPress);
    unpack(result.menu, command.heldButtons, kMenuHeld);
    unpack(result.menu, command.pressedButtons, kMenuPress);
    return result;
}

} // namespace gdl::game
