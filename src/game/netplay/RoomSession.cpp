#include "game/netplay/RoomSession.h"

#include <chrono>
#include <deque>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>

#include "game/netplay/RoomClient.h"

namespace gdl::game {

struct RoomSession::Impl {
    enum class Action : u8 { Ready, Start };
    struct Command {
        Action action = Action::Ready;
        u64 revision = 0;
        bool ready = true;
    };
    std::mutex mutex;
    Update update;
    std::deque<PeerTransport::Signal> outgoing;
    std::deque<Command> commands;
    bool leaving = false;
    bool done = false;
    // Last member: jthread stops/joins before any state it borrows is destroyed.
    std::jthread worker;

    Impl(std::string endpoint, std::string code, u8 localPlayers, std::string build,
         std::string contentHash)
        : worker([this, endpoint = std::move(endpoint), code = std::move(code), localPlayers,
                  build = std::move(build),
                  contentHash = std::move(contentHash)](const std::stop_token& stop) {
              run(stop, endpoint, code, localPlayers, build, contentHash);
          }) {}
    ~Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void publish(RoomSnapshot room, const std::string& peer) {
        const std::scoped_lock lock(mutex);
        if (update.room) {
            // Preserve unread signals while replacing only the latest roster.
            auto& previous = update.room->signals;
            room.signals.insert(room.signals.begin(), std::make_move_iterator(previous.begin()),
                                std::make_move_iterator(previous.end()));
            if (room.signals.size() > 64) {
                room.signals.erase(room.signals.begin(), room.signals.end() - 64);
            }
        }
        update.room = std::move(room);
        update.peer = peer;
    }

    void run(const std::stop_token& stop, const std::string& endpoint, const std::string& code,
             u8 localPlayers, const std::string& build, const std::string& contentHash) {
        try {
            RoomClient client(endpoint);
            publish(client.enter(code, localPlayers, build, contentHash), client.peer());
            while (!stop.stop_requested()) {
                std::deque<Command> control;
                std::deque<PeerTransport::Signal> signals;
                bool leave = false;
                {
                    const std::scoped_lock lock(mutex);
                    leave = leaving;
                    control.swap(commands);
                    // Service traffic cannot starve heartbeats/roster polling.
                    for (usize count = 0; count < 4 && !outgoing.empty(); ++count) {
                        signals.push_back(std::move(outgoing.front()));
                        outgoing.pop_front();
                    }
                }
                if (leave) {
                    client.leave();
                    break;
                }
                for (const auto& command : control) {
                    if (stop.stop_requested()) {
                        break;
                    }
                    try {
                        publish(command.action == Action::Ready
                                    ? client.ready(command.revision, command.ready)
                                    : client.start(command.revision),
                                client.peer());
                    } catch (const RoomServiceError& error) {
                        if (error.status() != 409) {
                            throw;
                        } // Stale roster: publish the fresh state below; never force start.
                    }
                }
                for (const auto& signal : signals) {
                    if (stop.stop_requested()) {
                        break;
                    }
                    try {
                        client.sendSignal(signal);
                    } catch (const RoomServiceError& error) {
                        if (error.status() != 403 && error.status() != 404 &&
                            error.status() != 429) {
                            throw;
                        } // Best-effort signaling to a departed peer, or backpressure.
                    }
                }
                if (!stop.stop_requested()) {
                    publish(client.poll(), client.peer());
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
        } catch (const std::exception& error) {
            const std::scoped_lock lock(mutex);
            update.error = error.what();
        }
        const std::scoped_lock lock(mutex);
        update.closed = true;
        done = true;
        outgoing.clear();
        commands.clear();
    }

    bool command(Action action, u64 revision, bool ready = true) {
        const std::scoped_lock lock(mutex);
        if (leaving || done || commands.size() >= 8) {
            return false;
        }
        commands.push_back({action, revision, ready});
        return true;
    }
};

RoomSession::RoomSession(std::string endpoint, std::string code, u8 localPlayers, std::string build,
                         std::string contentHash)
    : m_impl(std::make_unique<Impl>(std::move(endpoint), std::move(code), localPlayers,
                                    std::move(build), std::move(contentHash))) {}
RoomSession::~RoomSession() = default;
RoomSession::Update RoomSession::poll() {
    const std::scoped_lock lock(m_impl->mutex);
    return std::exchange(m_impl->update, {});
}
bool RoomSession::send(PeerTransport::Signal signal) {
    if (signal.bytes.empty() || signal.bytes.size() > PeerTransport::kMaxSignalBytes) {
        return false;
    }
    const std::scoped_lock lock(m_impl->mutex);
    if (m_impl->leaving || m_impl->done || m_impl->outgoing.size() >= 64) {
        return false;
    }
    m_impl->outgoing.push_back(std::move(signal));
    return true;
}
bool RoomSession::ready(u64 revision, bool value) {
    return m_impl->command(Impl::Action::Ready, revision, value);
}
bool RoomSession::start(u64 revision) {
    return m_impl->command(Impl::Action::Start, revision);
}
void RoomSession::leave() {
    const std::scoped_lock lock(m_impl->mutex);
    m_impl->leaving = true;
}

} // namespace gdl::game
