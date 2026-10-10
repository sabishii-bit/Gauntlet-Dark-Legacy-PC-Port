#include "game/screens/MatchInputs.h"

namespace gdl::game {
bool MatchInputs::sample(MatchSession& session, const SessionInputs::Frame& local) {
    const auto& context = session.context();
    std::vector<InputCommand> commands;
    for (usize seat = 0; seat < context.owners.size(); ++seat) {
        if (context.owners[seat] == session.localPeer() && session.localPeer() != 0) {
            commands.push_back(SessionInputs::command(local[commands.size()], context.epoch,
                                                      session.inputTick(), context.grants[seat],
                                                      static_cast<u8>(seat)));
        }
    }
    return session.sample(commands);
}
std::optional<SessionInputs::Frame> MatchInputs::advance(MatchSession& session) {
    const auto commands = session.advance();
    if (!commands) {
        return std::nullopt;
    }
    SessionInputs::Frame frame;
    for (usize seat = 0; seat < frame.size(); ++seat) {
        frame[seat] = SessionInputs::playInput((*commands)[seat]);
    }
    return frame;
}
} // namespace gdl::game
