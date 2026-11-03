#include "game/app/OnlineRun.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <future>

#include "engine/core/Log.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/SystemFont.h"

#if GDL_ENABLE_NETPLAY
#include "engine/net/SessionTransport.h"

#include "game/app/NativeOnlineIdentity.h"
#include "game/netplay/HostedRoom.h"
#include "game/netplay/OnlineParty.h"
#include "game/online/NativeOnlinePlay.h"
#include "game/rollback/RollbackInputs.h"
#endif

namespace gdl::game {
namespace {
constexpr u64 kNetworkLineTicks = 60; ///< one second at the native tick rate
constexpr u32 kNetworkTextPixels = 24;
constexpr s64 kNoticeSeconds = 5; ///< how long a one-off message such as a drop stays
} // namespace
struct OnlineRun::Impl {
#if GDL_ENABLE_NETPLAY
    RenderDevice* device = nullptr;
    GameContext context;
    OnlineLinks links;
    GameConfig config;
    LevelRef initial;
    PlayOptions options;
    std::unique_ptr<SessionTransport> transport;
    std::unique_ptr<HostedRoom> service;
    std::unique_ptr<OnlineSession> session;
    std::filesystem::path invitationOutput;
    bool invitationWritten = false;
    OnlineParty selection;
    NativeOnlinePlay nativePlay;
    std::unique_ptr<NativePlaySetup> nativeSetup;
    std::optional<NativeOnlineIdentity::Inputs> identityInputs;
    std::future<std::optional<NativeOnlineIdentity::Result>> identityResult;
    std::string build;
    std::string content;
    bool nativeConfigured = false;
    bool autoStart = false;
    bool interactive = false;
    bool selected = false;
    u8 localPlayers = 1;
    bool finished = false;
    u64 reportedEpoch = 0;
    u64 reportedTick = 0;
    u64 networkTick = 0;
    std::array<bool, 5> droppedSeen{};
    std::string notice;
    std::optional<std::chrono::steady_clock::time_point> noticeUntil;
    std::optional<u8> chosenLead;
#endif
};
OnlineRun::OnlineRun() = default;
bool OnlineRun::available() {
    return GDL_ENABLE_NETPLAY != 0;
}
OnlineRun::~OnlineRun() {
    close();
}
void OnlineRun::close() {
#if GDL_ENABLE_NETPLAY
    if (m_impl && m_impl->session) {
        m_impl->session->leave();
    }
#endif
    m_impl.reset();
    m_statusTexture.reset();
    m_status.clear();
    m_networkTexture.reset();
    m_network.clear();
    m_pingMs = -1;
}
std::optional<std::string> OnlineRun::stallNotice(f64 stalledSeconds) {
    if (stalledSeconds < kStallNoticeSeconds) {
        return std::nullopt;
    }
    const auto left =
        static_cast<s32>(std::ceil(std::max(kStallDeadlineSeconds - stalledSeconds, 0.0)));
    return std::format("Waiting for another player... {}s", left);
}
std::string OnlineRun::networkSummary(s32 pingMs) {
    return pingMs >= 0 ? std::format("{} ms", pingMs) : std::string("ping ?");
}
void OnlineRun::network(RenderDevice& device, std::string line, s32 pingMs) {
    m_pingMs = pingMs;
    if (line == m_network) {
        return;
    }
    m_network = std::move(line);
    if (m_network.empty()) {
        m_networkTexture.reset();
        return;
    }
    try {
        const auto image = rasterizeSystemText(m_network, kNetworkTextPixels);
        m_networkTexture = device.createTexture(
            {image.width, image.height, TextureFilter::Linear, TextureWrap::ClampToEdge},
            image.pixels);
    } catch (const std::exception& error) {
        m_networkTexture.reset();
        log::warn("Network line: {}", error.what());
    }
}
void OnlineRun::status(RenderDevice& device, std::string message) {
    if (message == m_status) {
        return;
    }
    m_status = std::move(message);
    log::info("Online run: {}", m_status);
    if (m_status.empty()) {
        m_statusTexture.reset();
        return;
    }
    try {
        const auto image = rasterizeSystemText(m_status, 32);
        m_statusTexture = device.createTexture(
            {image.width, image.height, TextureFilter::Linear, TextureWrap::ClampToEdge},
            image.pixels);
    } catch (const std::exception& error) {
        m_statusTexture.reset();
        log::warn("Online status: {}", error.what());
    }
}
bool OnlineRun::open(RenderDevice& device, const GameContext& context, const Links& links,
                     const std::string& endpoint, const std::string& code, const std::string& build,
                     const std::string& content, std::span<const PartyMember> local,
                     const LevelRef& initial, const PlayOptions& options, bool autoStart,
                     const std::filesystem::path& invitationOutput) {
    close();
#if GDL_ENABLE_NETPLAY
    if (local.empty() || local.size() > InputCommand::kSeats) {
        status(device, "Online test requires one to four local characters.");
        return false;
    }
    if (!invitationOutput.empty() && std::filesystem::exists(invitationOutput)) {
        status(device, "Invitation output already exists; choose a new file.");
        return false;
    }
    std::string invitation;
    if (!code.empty()) {
        FileStream file(code);
        if (file.size() == 0 || file.size() > 2048) {
            status(device, "Invalid invitation file.");
            return false;
        }
        const auto bytes = file.readExact(static_cast<usize>(file.size()));
        invitation.assign(bytes.begin(), bytes.end());
    }
    if (!connect(device, context, links, endpoint == "local", invitation, build, content,
                 static_cast<u8>(local.size()))) {
        return false;
    }
    m_impl->initial = initial;
    m_impl->options = options;
    m_impl->autoStart = autoStart;
    m_impl->invitationOutput = invitationOutput;
    return select(local);
#else
    (void)endpoint;
    (void)code;
    (void)local;
    (void)initial;
    (void)options;
    (void)autoStart;
    (void)invitationOutput;
    return connect(device, context, links, false, {}, build, content, 1);
#endif
}
bool OnlineRun::connect(RenderDevice& device, const GameContext& context, const Links& links,
                        bool localOnly, const std::string& invitation, const std::string& build,
                        const std::string& content, u8 localPlayers) {
    close();
#if GDL_ENABLE_NETPLAY
    if (localPlayers == 0 || localPlayers > InputCommand::kSeats) {
        status(device, "Choose one to four local players.");
        return false;
    }
    auto next = std::make_unique<Impl>();
    next->device = &device;
    next->context = context;
    next->links = links;
    next->config = context.config != nullptr ? *context.config : GameConfig{};
    next->context.config = &next->config;
    next->initial = LevelRef::tower();
    next->localPlayers = localPlayers;
    if (context.strings == nullptr || context.levels == nullptr || !context.levels->loaded()) {
        status(device, "Online simulation requires loaded game text and levels.");
        return false;
    }
    std::string identityError;
    next->identityInputs = NativeOnlineIdentity::snapshot(*context.strings, &identityError);
    if (!next->identityInputs) {
        status(device, "Online compatibility check failed: " + identityError);
        return false;
    }
    next->build = build;
    next->content = content;
    next->identityResult = std::async(std::launch::async, [inputs = *next->identityInputs] {
        return NativeOnlineIdentity::capture(inputs);
    });
    std::string error;
    next->transport = openSessionTransport({localOnly, invitation}, error);
    if (!next->transport) {
        status(device, "Network startup failed: " + error);
        return false;
    }
    next->service = std::make_unique<HostedRoom>(*next->transport, invitation.empty(), localPlayers,
                                                 build, content);
    next->session = std::make_unique<OnlineSession>(*next->service, next->service->transport(),
                                                    invitation.empty(), localPlayers);
    if (invitation.empty()) {
        // The host's room opens on the camera rule it was last hosted with and the
        // mechanics the host plays with; all of them are the room's to change.
        RoomSettings seeded;
        seeded.camera = next->config.multiplayer.soloCamera ? 1 : 0;
        seeded.mechanics =
            context.mechanics != nullptr ? context.mechanics->roomBits(next->config.game) : 0;
        next->service->settings(seeded);
    }
    m_impl = std::move(next);
    status(device, "Connecting...");
    return true;
#else
    (void)context;
    (void)links;
    (void)localOnly;
    (void)invitation;
    (void)build;
    (void)content;
    (void)localPlayers;
    status(device, "This build does not include the experimental netplay runtime.");
    return false;
#endif
}
bool OnlineRun::openLobby(RenderDevice& device, const GameContext& context, const Links& links,
                          const std::string& invitation, const std::string& build,
                          const std::string& content, u8 localPlayers, bool localOnly) {
    if (!connect(device, context, links, localOnly, invitation, build, content, localPlayers)) {
        return false;
    }
#if GDL_ENABLE_NETPLAY
    m_impl->interactive = true;
#endif
    return true;
}
bool OnlineRun::select(std::span<const PartyMember> local) {
#if GDL_ENABLE_NETPLAY
    if (m_impl && local.size() == m_impl->localPlayers &&
        m_impl->selection.select(*m_impl->session, local)) {
        m_impl->selected = true;
        return true;
    }
#else
    (void)local;
#endif
    return false;
}
bool OnlineRun::ready(bool value) {
#if GDL_ENABLE_NETPLAY
    return m_impl && m_impl->nativeConfigured && m_impl->session->ready(value);
#else
    (void)value;
    return false;
#endif
}
bool OnlineRun::start() {
#if GDL_ENABLE_NETPLAY
    return m_impl && m_impl->session->start();
#else
    return false;
#endif
}
bool OnlineRun::settings(const RoomSettings& value) {
#if GDL_ENABLE_NETPLAY
    if (!m_impl) {
        return false;
    }
    const auto* offered = m_impl->context.mechanics;
    const u8 rules = offered != nullptr ? offered->knownRoomBits() : 0;
    return (value.mechanics & ~rules) == 0 && m_impl->service->settings(value);
#else
    (void)value;
    return false;
#endif
}
NetplayMenu::View OnlineRun::lobby() const {
    NetplayMenu::View view;
    view.status = m_status;
#if GDL_ENABLE_NETPLAY
    if (m_impl) {
        const auto& run = *m_impl;
        view.canSaveCompletedProgress = run.nativePlay.hasCompletedProgress();
        if (run.finished || run.nativePlay.phase() == NativeOnlinePlay::Phase::Finished ||
            run.nativePlay.phase() == NativeOnlinePlay::Phase::Failed ||
            run.session->phase() == OnlineSession::Phase::Failed) {
            view.page = NetplayMenu::Page::Finished;
            return view;
        }
        view.page = run.session->host() ? NetplayMenu::Page::Host : NetplayMenu::Page::Guest;
        view.host = run.session->host();
        view.localPlayers = run.localPlayers;
        view.selected = run.selected;
        view.connected =
            run.session->phase() == OnlineSession::Phase::Lobby && run.nativeConfigured;
        if (run.session->host() && run.transport->phase() == SessionTransport::Phase::Ready) {
            view.invitation = run.transport->invitation();
        }
        if (run.context.mechanics != nullptr && run.context.strings != nullptr) {
            for (const MechanicDescriptor* rule : run.context.mechanics->roomRules()) {
                view.rules.emplace_back(run.context.strings->get(rule->label));
            }
        }
        if (const auto* room = run.session->room()) {
            view.settings = room->settings;
            bool allReady = true;
            for (const auto& member : room->members) {
                view.players += member.seats.size();
                allReady &= member.ready;
                if (std::ranges::equal(member.seats, run.session->seats())) {
                    view.ready = member.ready;
                }
            }
            view.canStart =
                run.session->host() && view.connected && allReady && run.session->connected();
        }
    }
#endif
    return view;
}
bool OnlineRun::playing() const {
#if GDL_ENABLE_NETPLAY
    if (m_impl && !m_impl->finished) {
        const auto phase = m_impl->nativePlay.phase();
        return phase == NativeOnlinePlay::Phase::Loading ||
               phase == NativeOnlinePlay::Phase::Resyncing ||
               phase == NativeOnlinePlay::Phase::Playing ||
               phase == NativeOnlinePlay::Phase::Shopping ||
               phase == NativeOnlinePlay::Phase::Paused;
    }
    return false;
#else
    return false;
#endif
}
bool OnlineRun::pause(s32 device) {
#if GDL_ENABLE_NETPLAY
    return m_impl && m_impl->nativePlay.pause(device);
#else
    (void)device;
    return false;
#endif
}
u64 OnlineRun::simulatedTick() const {
#if GDL_ENABLE_NETPLAY
    return m_impl ? m_impl->nativePlay.simulatedTick() : 0;
#else
    return 0;
#endif
}
u64 OnlineRun::confirmedTick() const {
#if GDL_ENABLE_NETPLAY
    return m_impl ? m_impl->nativePlay.confirmedTick() : 0;
#else
    return 0;
#endif
}
usize OnlineRun::directLinks() const {
#if GDL_ENABLE_NETPLAY
    return m_impl && m_impl->session ? m_impl->session->directLinks() : 0;
#else
    return 0;
#endif
}
void OnlineRun::resume() {
#if GDL_ENABLE_NETPLAY
    if (m_impl) {
        m_impl->nativePlay.resume();
    }
#endif
}
bool OnlineRun::hasCompletedProgress() const {
#if GDL_ENABLE_NETPLAY
    return m_impl && m_impl->nativePlay.hasCompletedProgress();
#else
    return false;
#endif
}
bool OnlineRun::saveCompletedProgress() {
#if GDL_ENABLE_NETPLAY
    return m_impl && m_impl->nativePlay.saveCompletedProgress();
#else
    return false;
#endif
}
void OnlineRun::update(const SessionInputs::Frame& devices, const MenuInput& menu) {
    if (!m_impl) {
        return;
    }
#if GDL_ENABLE_NETPLAY
    auto& run = *m_impl;
    auto& session = *run.session;
    if (run.transport->phase() == SessionTransport::Phase::Opening) {
        return; // Connection setup has its own deadline, before room admission begins.
    }
    if (run.transport->phase() == SessionTransport::Phase::Failed) {
        status(*run.device, "Connection failed: " + run.transport->error());
        session.leave();
        run.finished = true;
        return;
    }
    if (!run.finished && session.host() && !run.invitationWritten &&
        !run.invitationOutput.empty()) {
        try {
            replaceTextFile(run.invitationOutput, run.transport->invitation());
            // This harness file is private. Never include the invitation in logs.
            log::info("Online invitation ready");
            run.invitationWritten = true;
        } catch (const std::exception&) {
            status(*run.device, "Connection failed: unable to save the invitation file.");
            session.leave();
            run.finished = true;
            return;
        }
    }
    if (run.finished) {
        session.update(1.0 / 60);
        return;
    }
    const bool closed = run.nativePlay.phase() == NativeOnlinePlay::Phase::Closed;
    if (closed) {
        session.update(1.0 / 60);
        if (!run.nativeConfigured) {
            if (!run.identityResult.valid() ||
                run.identityResult.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                status(*run.device, "Checking simulation compatibility...");
                return;
            }
            const auto identity = run.identityResult.get();
            std::optional<OnlineSession::NativeRules> rules;
            if (session.host()) {
                rules = OnlineSession::NativeRules{};
                const auto scene = NativeOnlinePlay::sceneId(*run.context.levels, run.initial);
                if (!scene) {
                    status(*run.device, "Online initial stage is not in the native catalog.");
                    session.leave();
                    run.finished = true;
                    return;
                }
                rules->scene = *scene;
                rules->autoMelee = run.config.combat.autoMelee;
                rules->autoActivateItems = run.config.combat.autoActivateItems;
                rules->expandedShop = run.config.game.expandedShop;
                rules->autoAim = run.config.combat.autoAim;
            }
            if (!identity || !run.identityInputs ||
                !NativeOnlineIdentity::numericMatches(*run.identityInputs) ||
                !session.configureNative(
                    {run.build, identity->runtime, run.content, identity->data, identity->language},
                    rules)) {
                status(*run.device, "Online simulation compatibility check failed. Escape exits.");
                session.leave();
                run.finished = true;
                return;
            }
            run.nativeConfigured = true;
        }
        if (session.nativeSetupPending()) {
            const auto* contract = session.nativeContract();
            const auto* profiles = session.party();
            if (contract != nullptr && profiles != nullptr && session.room() != nullptr) {
                const auto& rules = contract->rules;
                const auto& identity = contract->identity;
                // The loaded text may have fallen back from the requested locale.
                // Agreement binds the resolved table, not an unavailable preference.
                run.config.text.language = identity.language;
                run.nativeSetup = NativePlaySetup::create(
                    run.config, session.room()->settings, *profiles, run.options,
                    {rules.scene, rules.autoMelee, rules.autoActivateItems, rules.expandedShop,
                     rules.autoAim, identity.build, identity.content, identity.data,
                     run.context.mechanics});
            }
            if (!run.nativeSetup || !session.confirmNativeSetup(run.nativeSetup->digest())) {
                status(*run.device, "Online party setup disagrees. Escape exits.");
                session.leave();
                run.finished = true;
                return;
            }
        }
        if (session.phase() == OnlineSession::Phase::Lobby) {
            const auto* room = session.room();
            if (room != nullptr) {
                // A changed lobby roster clears service readiness. Reassert it
                // for these immutable test selections; the real lobby uses UI.
                const auto own = std::ranges::find_if(room->members, [&](const auto& member) {
                    return member.seats ==
                           std::vector<u8>(session.seats().begin(), session.seats().end());
                });
                if (!run.interactive && own != room->members.end() && !own->ready) {
                    session.ready();
                }
                if (run.interactive) {
                    status(*run.device, run.selected
                                            ? "Ready when you are."
                                            : "Select your characters, then mark yourself ready.");
                } else {
                    status(*run.device, std::format("Test room {} - {} machine(s). {}", room->code,
                                                    room->members.size(),
                                                    session.host() ? "Press Start to begin."
                                                                   : "Waiting for host."));
                }
                if (!run.interactive && session.host() && room->members.size() > 1 &&
                    (run.autoStart || menu.start || menu.select)) {
                    // The delay every peer will buffer its own inputs by comes from the
                    // slowest lobby link; prediction covers what is left.
                    const auto link = session.linkStatistics();
                    const u8 lead = RollbackInputs::leadForPing(link ? link->pingMs : -1);
                    if (session.adoptStartupDelay(lead) && run.chosenLead != lead) {
                        run.chosenLead = lead;
                        log::info("Online input delay {} tick(s) for a {} ms round trip", lead,
                                  link ? link->pingMs : -1);
                    }
                    session.start();
                }
            }
        } else if (session.phase() == OnlineSession::Phase::Active) {
            const auto rules = session.room()->settings;
            run.config.difficulty.level = DifficultyConfig::kNames[rules.difficulty];
            run.config.multiplayer.mode = static_cast<MultiplayerMode>(rules.friendlyFire);
            run.config.multiplayer.soloCamera = rules.camera == 1;
            applyRoomRules(run.context.mechanics, rules.mechanics, run.config.game);
            const bool opened =
                run.identityInputs && NativeOnlineIdentity::numericMatches(*run.identityInputs) &&
                run.nativePlay.open(*run.device, run.context, session, run.selection,
                                    std::move(run.nativeSetup), run.links);
            if (!opened) {
                status(*run.device, "Online scene setup failed. Escape exits.");
                session.leave();
                run.finished = true;
            } else {
                status(*run.device, "Loading the shared stage...");
            }
        }
    } else {
        if (!run.interactive && menu.start) {
            if (run.nativePlay.phase() == NativeOnlinePlay::Phase::Paused) {
                run.nativePlay.resume();
            } else {
                run.nativePlay.pause();
            }
        }
        // Silence is judged as of the frame's start: while a frame loads or rebuilds, this
        // machine hears nobody, and that is not the others' silence.
        const auto now = std::chrono::steady_clock::now();
        const auto phase = run.nativePlay.update(devices);
        const auto completed = run.nativePlay.simulatedTick();
        const auto epoch = session.match().context().epoch;
        if ((phase == NativeOnlinePlay::Phase::Playing ||
             phase == NativeOnlinePlay::Phase::Paused) &&
            completed > 0 &&
            (epoch != run.reportedEpoch || completed / 300 > run.reportedTick / 300)) {
            run.reportedEpoch = epoch;
            run.reportedTick = completed;
            log::info("Native online progress: epoch {} tick {} ({})", epoch, completed,
                      session.host() ? "host" : "guest");
            const auto stats = run.nativePlay.rollbackStatistics();
            log::info("Native rollback: {} native ticks, {} replayed, {} corrections (longest "
                      "replay {}), {} stalls, {} pacing idles, confirmed tick {}",
                      stats.nativeTicks, stats.replayTicks, stats.corrections, stats.maxReplay,
                      stats.stalls, stats.pacingIdles, run.nativePlay.confirmedTick());
        }
        if ((phase == NativeOnlinePlay::Phase::Playing ||
             phase == NativeOnlinePlay::Phase::Paused) &&
            completed / kNetworkLineTicks != run.networkTick / kNetworkLineTicks) {
            run.networkTick = completed;
            const auto link = session.linkStatistics();
            const s32 ping = link ? link->pingMs : -1;
            network(*run.device, networkSummary(ping), ping);
        } else if (phase != NativeOnlinePlay::Phase::Playing &&
                   phase != NativeOnlinePlay::Phase::Paused) {
            network(*run.device, "", -1);
        }
        // A peer that stops sending starves the simulation, however the pacing hides it: say
        // so after a moment, and at the deadline the host drops whoever went quiet and the
        // game goes on; a guest leaves only when it is the host that went quiet.
        std::optional<std::string> stall;
        const bool resyncing = session.match().resyncing();
        if ((phase == NativeOnlinePlay::Phase::Playing ||
             phase == NativeOnlinePlay::Phase::Paused) &&
            !resyncing) {
            const auto quiet = session.match().quietPeers(kStallNoticeSeconds, now);
            const f64 seconds = session.match().quietSeconds(now);
            if (!quiet.empty() && seconds >= kStallDeadlineSeconds) {
                bool dropped = false;
                if (session.host()) {
                    for (const u8 peer : session.match().quietPeers(kStallDeadlineSeconds, now)) {
                        dropped = session.match().drop(peer) || dropped;
                    }
                }
                const bool hostQuiet = std::ranges::find(quiet, u8{1}) != quiet.end();
                if (!dropped && (session.host() || hostQuiet)) {
                    status(*run.device, "Another player stopped responding. Online session "
                                        "ended. Escape exits.");
                    session.leave();
                    run.finished = true;
                    return;
                }
            }
            if (!session.match().quietPeers(kStallNoticeSeconds, now).empty()) {
                stall = stallNotice(session.match().quietSeconds(now));
            }
        }
        // Whoever dropped out, at the deadline above or by losing its link, the game goes on.
        bool newlyDropped = false;
        for (u8 peer = 2; peer <= 4; ++peer) {
            const bool gone = session.match().dropped(peer);
            newlyDropped = newlyDropped || (gone && !run.droppedSeen[peer]);
            run.droppedSeen[peer] = gone;
        }
        if (newlyDropped) {
            run.notice = "A player dropped out of the online game. The game goes on.";
            run.noticeUntil = now + std::chrono::seconds(kNoticeSeconds);
        }
        if (stall) {
            status(*run.device, *stall);
            return;
        }
        if (resyncing && phase == NativeOnlinePlay::Phase::Playing) {
            status(*run.device, "Waiting for a player to resynchronise...");
            return;
        }
        switch (phase) {
        case NativeOnlinePlay::Phase::Loading:
            status(*run.device, "Loading the shared stage...");
            break;
        case NativeOnlinePlay::Phase::Playing:
        case NativeOnlinePlay::Phase::Shopping:
            status(*run.device,
                   run.noticeUntil && now < *run.noticeUntil ? run.notice : std::string{});
            break;
        case NativeOnlinePlay::Phase::Paused:
            status(*run.device, "Local menu open - the game continues for everyone.");
            break;
        case NativeOnlinePlay::Phase::Resyncing:
            status(*run.device, "Resynchronising with the host...");
            break;
        case NativeOnlinePlay::Phase::Finished:
            // Keep the final input relay alive until the user leaves. Another
            // peer may still be executing this campaign's final native tick.
            status(*run.device, run.nativePlay.outcome() == NativeRollbackDriver::Status::Leave
                                    ? "A player left. Online session ended. Escape exits."
                                    : "Online game finished. Escape exits.");
            break;
        case NativeOnlinePlay::Phase::Failed: {
            const auto detail = run.nativePlay.failureDetail();
            status(*run.device,
                   std::format("{}{} Escape exits.", detail, detail.ends_with('.') ? "" : "."));
            run.finished = true;
            break;
        }
        default: break;
        }
    }
    if (!run.finished && session.phase() == OnlineSession::Phase::Failed) {
        status(*run.device, session.failureDetail().empty()
                                ? std::format("Connection failed ({}). Escape exits.",
                                              static_cast<u32>(session.failure()))
                                : std::format("{}{} Escape exits.", session.failureDetail(),
                                              session.failureDetail().ends_with('.') ? "" : "."));
    }
#else
    (void)devices;
    (void)menu;
#endif
}
std::optional<Vec3> OnlineRun::cursorAim(s32 device, Vec2 cursor) const {
    if (!m_impl) {
        return std::nullopt;
    }
#if GDL_ENABLE_NETPLAY
    const auto& run = *m_impl;
    for (const auto seat : run.session->seats()) {
        if (run.selection.device(*run.session, seat) != device) {
            continue;
        }
        if (const auto* scene = run.nativePlay.scene()) {
            if (const auto* actor = scene->actor(seat)) {
                return scene->cursorAim(cursor, actor->position());
            }
        }
    }
#else
    (void)device;
    (void)cursor;
#endif
    return std::nullopt;
}
bool OnlineRun::consoleHost() const {
#if GDL_ENABLE_NETPLAY
    return m_impl && m_impl->session && m_impl->session->host();
#else
    return false;
#endif
}
bool OnlineRun::consoleAllowed() const {
#if GDL_ENABLE_NETPLAY
    if (!m_impl || !m_impl->session) {
        return false;
    }
    const auto& match = m_impl->session->match();
    const auto phase = m_impl->nativePlay.phase();
    return (phase == NativeOnlinePlay::Phase::Playing ||
            phase == NativeOnlinePlay::Phase::Paused) &&
           match.phase() == MatchSession::Phase::Running && !match.resyncing() &&
           !match.nativeDraining() && (match.host() || match.consolePermissions());
#else
    return false;
#endif
}
bool OnlineRun::setConsolePermissions(bool enabled) {
#if GDL_ENABLE_NETPLAY
    return consoleHost() && m_impl->session->match().setConsolePermissions(enabled);
#else
    (void)enabled;
    return false;
#endif
}
bool OnlineRun::consoleCommand(u8 seat, std::string line) {
#if GDL_ENABLE_NETPLAY
    return consoleAllowed() &&
           m_impl->session->match().requestConsole(ConsoleAction{seat, std::move(line)});
#else
    (void)seat;
    (void)line;
    return false;
#endif
}
const PlayScene* OnlineRun::consoleScene() const {
#if GDL_ENABLE_NETPLAY
    return m_impl ? m_impl->nativePlay.scene() : nullptr;
#else
    return nullptr;
#endif
}
const LevelWorld* OnlineRun::consoleWorld() const {
#if GDL_ENABLE_NETPLAY
    return m_impl ? m_impl->nativePlay.world() : nullptr;
#else
    return nullptr;
#endif
}
std::optional<u8> OnlineRun::consoleSeat(s32 device) const {
#if GDL_ENABLE_NETPLAY
    if (m_impl && m_impl->session) {
        for (u8 seat : m_impl->session->seats()) {
            if (m_impl->selection.device(*m_impl->session, seat) == device) {
                return seat;
            }
        }
    }
#else
    (void)device;
#endif
    return std::nullopt;
}
void OnlineRun::consoleCapture(bool captured) {
#if GDL_ENABLE_NETPLAY
    if (m_impl) {
        m_impl->nativePlay.consoleCapture(captured);
    }
#else
    (void)captured;
#endif
}
void OnlineRun::consoleView(bool hideHud, const std::optional<WorldCamera>& camera) {
#if GDL_ENABLE_NETPLAY
    if (m_impl) {
        m_impl->nativePlay.consoleView(hideHud, camera);
    }
#else
    (void)hideHud;
    (void)camera;
#endif
}
void OnlineRun::consoleSelect(const std::optional<SceneSelection>& selection) {
#if GDL_ENABLE_NETPLAY
    if (m_impl) {
        m_impl->nativePlay.consoleSelect(selection);
    }
#else
    (void)selection;
#endif
}
std::vector<std::string> OnlineRun::takeConsoleReplies() {
#if GDL_ENABLE_NETPLAY
    return m_impl ? m_impl->nativePlay.takeConsoleReplies() : std::vector<std::string>{};
#else
    return {};
#endif
}
std::optional<SceneSelection> OnlineRun::consolePick(Vec2 cursor) {
#if GDL_ENABLE_NETPLAY
    return m_impl ? m_impl->nativePlay.consolePick(cursor) : std::nullopt;
#else
    (void)cursor;
    return std::nullopt;
#endif
}
void OnlineRun::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height,
                       f64 seconds, f32 frameBlend) {
#if GDL_ENABLE_NETPLAY
    if (m_impl) {
        m_impl->nativePlay.render(device, projection, width, height, seconds, frameBlend);
        if (m_impl->nativePlay.entryVisible()) {
            return;
        }
    }
#else
    (void)seconds;
    (void)frameBlend;
#endif
    if (m_networkTexture != nullptr) {
        Canvas canvas;
        canvas.begin(device, projection, {.depthWrite = false, .depthTest = false});
        const auto textWidth = static_cast<f32>(m_networkTexture->width()) / 2;
        const auto textHeight = static_cast<f32>(m_networkTexture->height()) / 2;
        // The signal mark sits above the ping, both against the panel's right edge.
        const auto mark = LinkIndicator::size();
        const f32 inner = std::max(textWidth, mark.x);
        canvas.fill({width - inner - 12, 4, inner + 8, mark.y + textHeight + 6},
                    Color::rgba(0, 0, 0, 160));
        LinkIndicator::draw(canvas, {width - mark.x - 8, 6}, LinkIndicator::quality(m_pingMs));
        canvas.draw(*m_networkTexture,
                    {width - textWidth - 8, 6 + mark.y + 2, textWidth, textHeight});
        canvas.end();
    }
    if (m_statusTexture != nullptr) {
        Canvas canvas;
        canvas.begin(device, projection, {.depthWrite = false, .depthTest = false});
        const f32 textWidth = std::min(width - 16, static_cast<f32>(m_statusTexture->width()) / 2);
        const f32 textHeight = textWidth * static_cast<f32>(m_statusTexture->height()) /
                               static_cast<f32>(m_statusTexture->width());
        canvas.fill({4, height - textHeight - 12, width - 8, textHeight + 8},
                    Color::rgba(0, 0, 0, 220));
        canvas.draw(*m_statusTexture, {8, height - textHeight - 8, textWidth, textHeight});
        canvas.end();
    }
}
} // namespace gdl::game
