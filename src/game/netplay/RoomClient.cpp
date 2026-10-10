#include "game/netplay/RoomClient.h"

#include <algorithm>
#include <array>
#include <regex>
#include <set>
#include <stdexcept>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace gdl::game {
namespace {

using Json = nlohmann::json;

bool hexString(const std::string& text, usize length) {
    return text.size() == length && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Invalid room service response");
    }
}

RoomSnapshot snapshot(const Json& response) {
    const auto& room = response.at("room");
    RoomSnapshot result;
    result.code = room.at("code").get<std::string>();
    result.host = room.at("host").get<std::string>();
    require(result.code.size() == 8 && std::ranges::all_of(result.code, [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9');
            }));
    require(room.at("revision").is_number_unsigned());
    result.revision = room.at("revision").get<u64>();
    require(result.revision != 0);
    result.started = room.at("started").get<bool>();
    const auto& members = room.at("members");
    require(members.is_array() && !members.empty() && members.size() <= 4);
    std::array<bool, 4> occupied{};
    std::set<std::string> peers;
    for (const auto& row : members) {
        RoomMember member;
        member.peer = row.at("peer").get<std::string>();
        member.ready = row.at("ready").get<bool>();
        require(hexString(member.peer, 32) && peers.insert(member.peer).second);
        const auto& seats = row.at("seats");
        require(seats.is_array() && !seats.empty() && seats.size() <= 4);
        for (const auto& seat : seats) {
            require(seat.is_number_unsigned() && seat.get<u64>() < occupied.size());
            const auto index = seat.get<u8>();
            require(!occupied[index]);
            occupied[index] = true;
            member.seats.push_back(index);
        }
        result.members.push_back(std::move(member));
    }
    require(peers.contains(result.host));
    if (response.contains("signals")) {
        const auto& signals = response.at("signals");
        require(signals.is_array() && signals.size() <= 16);
        for (const auto& row : signals) {
            PeerTransport::Signal signal;
            signal.peer = row.at("peer").get<std::string>();
            const auto data = row.at("data").get<std::string>();
            require(peers.contains(signal.peer) && !data.empty() && data.size() % 2 == 0 &&
                    data.size() <= PeerTransport::kMaxSignalBytes * 2 &&
                    hexString(data, data.size()));
            const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            for (usize i = 0; i < data.size(); i += 2) {
                signal.bytes.push_back(static_cast<u8>(nibble(data[i]) * 16 + nibble(data[i + 1])));
            }
            result.signals.push_back(std::move(signal));
        }
    }
    return result;
}

} // namespace

struct RoomClient::Impl {
    httplib::Client http;
    std::string code;
    std::string peer;
    std::string token;
    std::string host;
    u64 revision = 0;
    bool started = false;
    explicit Impl(const std::string& endpoint) : http(endpoint) {
        http.set_connection_timeout(2);
        http.set_read_timeout(2);
        http.set_write_timeout(2);
        http.set_max_timeout(3000);
        http.set_payload_max_length(300000);
        http.set_follow_location(false); // Never redirect bearer credentials.
        http.enable_server_certificate_verification(true);
    }

    Json request(const std::string& action, Json body) {
        if (!code.empty()) {
            body["code"] = code;
        }
        httplib::Headers headers;
        if (!token.empty()) {
            headers.emplace("Authorization", "Bearer " + token);
        }
        const auto response = http.Post("/v1/" + action, headers, body.dump(), "application/json");
        if (!response) {
            throw std::runtime_error("Room service is unavailable");
        }
        if (response->status != 200) {
            // Do not echo arbitrary server text, response bodies or credentials.
            throw RoomServiceError(response->status);
        }
        try {
            return Json::parse(response->body,
                               [](s32 depth, Json::parse_event_t /*unused*/, Json& /*unused*/) {
                                   require(depth <= 16);
                                   return true;
                               });
        } catch (const Json::exception&) {
            throw std::runtime_error("Malformed room service response");
        }
    }

    RoomSnapshot read(const Json& response) {
        auto room = snapshot(response);
        require(room.code == code && room.host == host && room.revision >= revision &&
                (!started || room.started) &&
                std::ranges::any_of(room.members,
                                    [&](const auto& member) { return member.peer == peer; }));
        revision = room.revision;
        started = room.started;
        return room;
    }
};

RoomServiceError::RoomServiceError(s32 status)
    : std::runtime_error("Room request rejected (HTTP " + std::to_string(status) + ")"),
      m_status(status) {}

RoomClient::RoomClient(const std::string& endpoint) {
    // No paths, queries, userinfo, fragments or unencrypted remote endpoints.
    // Production DNS/TLS/CA policy belongs to the eventual service deployment.
    const std::regex secure(R"(https://[A-Za-z0-9.-]+(:[0-9]{1,5})?)");
    const std::regex local(R"(http://127\.0\.0\.1:[0-9]{1,5})");
    if (!std::regex_match(endpoint, secure) && !std::regex_match(endpoint, local)) {
        throw std::runtime_error("Room service requires HTTPS (HTTP allowed only on loopback)");
    }
    m_impl = std::make_unique<Impl>(endpoint);
}
RoomClient::~RoomClient() = default;

RoomSnapshot RoomClient::enter(const std::string& code, u8 localPlayers, const std::string& build,
                               const std::string& contentHash) {
    if (!m_impl->code.empty()) {
        throw std::runtime_error("Already admitted to a room");
    }
    Json body{
        {"players", localPlayers},
        {"compatibility", {{"protocol", kProtocol}, {"build", build}, {"content", contentHash}}}};
    if (!code.empty()) {
        body["code"] = code;
    }
    const auto response = m_impl->request(code.empty() ? "create" : "join", std::move(body));
    auto room = snapshot(response);
    const auto peer = response.at("peer").get<std::string>();
    const auto token = response.at("token").get<std::string>();
    require(hexString(peer, 32) && hexString(token, 64) && !room.started &&
            (code.empty() ? room.host == peer : room.code == code) &&
            std::ranges::any_of(room.members, [&](const auto& member) {
                return member.peer == peer && member.seats.size() == localPlayers;
            }));
    m_impl->code = room.code;
    m_impl->peer = peer;
    m_impl->token = token;
    m_impl->host = room.host;
    m_impl->revision = room.revision;
    m_impl->started = room.started;
    return room;
}

RoomSnapshot RoomClient::poll() {
    return m_impl->read(m_impl->request("poll", Json::object()));
}
RoomSnapshot RoomClient::ready(u64 revision, bool ready) {
    return m_impl->read(m_impl->request("ready", {{"revision", revision}, {"ready", ready}}));
}
RoomSnapshot RoomClient::start(u64 revision) {
    return m_impl->read(m_impl->request("start", {{"revision", revision}}));
}
void RoomClient::leave() {
    m_impl->request("leave", Json::object());
    m_impl->code.clear();
    m_impl->peer.clear();
    m_impl->token.clear();
}
void RoomClient::sendSignal(const PeerTransport::Signal& signal) {
    if (signal.bytes.empty() || signal.bytes.size() > PeerTransport::kMaxSignalBytes) {
        throw std::runtime_error("Invalid signaling payload size");
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string data;
    data.reserve(signal.bytes.size() * 2);
    for (const auto byte : signal.bytes) {
        data += kHex[byte >> 4U];
        data += kHex[byte & 15U];
    }
    m_impl->request("signal", {{"peer", signal.peer}, {"data", std::move(data)}});
}
const std::string& RoomClient::peer() const {
    return m_impl->peer;
}

} // namespace gdl::game
