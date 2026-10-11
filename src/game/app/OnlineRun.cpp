#include "game/app/OnlineRun.h"

#include <algorithm>
#include <format>

#include "engine/core/Log.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/SystemFont.h"

#if GDL_ENABLE_NETPLAY
#include "engine/net/SessionTransport.h"

#include "game/netplay/HostedRoom.h"
#include "game/screens/OnlinePlay.h"
#endif

namespace gdl::game {
struct OnlineRun::Impl {
#if GDL_ENABLE_NETPLAY
    RenderDevice* device = nullptr;
    GameContext context;
    GameConfig config;
    LevelRef initial;
    PlayOptions options;
    std::unique_ptr<SessionTransport> transport;
    std::unique_ptr<HostedRoom> service;
    std::unique_ptr<OnlineSession> session;
    std::filesystem::path invitationOutput;
    bool invitationWritten = false;
    OnlineParty selection;
    OnlinePlay play;
    bool autoStart = false;
    bool interactive = false;
    bool selected = false;
    u8 localPlayers = 1;
    bool finished = false;
    bool reportedCheckpoint = false;
    u64 reportedEpoch = 0;
    u64 reportedTick = 0;
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
}
void OnlineRun::status(RenderDevice& device, std::string message) {
    if (message == m_status) {
        return;
    }
    m_status = std::move(message);
    log::info("Online run: {}", m_status);
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
bool OnlineRun::open(RenderDevice& device, const GameContext& context, const std::string& endpoint,
                     const std::string& code, const std::string& build, const std::string& content,
                     std::span<const PartyMember> local, const LevelRef& initial,
                     const PlayOptions& options, bool autoStart,
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
    if (!connect(device, context, endpoint == "local", invitation, build, content,
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
    return connect(device, context, false, {}, build, content, 1);
#endif
}
bool OnlineRun::connect(RenderDevice& device, const GameContext& context, bool localOnly,
                        const std::string& invitation, const std::string& build,
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
    next->config = context.config != nullptr ? *context.config : GameConfig{};
    next->context.config = &next->config;
    next->initial = LevelRef::tower();
    next->options.welcome = false;
    next->localPlayers = localPlayers;
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
    m_impl = std::move(next);
    status(device, "Connecting...");
    return true;
#else
    (void)context;
    (void)localOnly;
    (void)invitation;
    (void)build;
    (void)content;
    (void)localPlayers;
    status(device, "This build does not include the experimental netplay runtime.");
    return false;
#endif
}
bool OnlineRun::openLobby(RenderDevice& device, const GameContext& context,
                          const std::string& invitation, const std::string& build,
                          const std::string& content, u8 localPlayers, bool localOnly) {
    if (!connect(device, context, localOnly, invitation, build, content, localPlayers)) {
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
    return m_impl && m_impl->session->ready(value);
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
    return m_impl && m_impl->service->settings(value);
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
        view.page = run.session->host() ? NetplayMenu::Page::Host : NetplayMenu::Page::Guest;
        view.localPlayers = run.localPlayers;
        view.selected = run.selected;
        view.connected = run.session->phase() == OnlineSession::Phase::Lobby;
        if (run.session->host() && run.transport->phase() == SessionTransport::Phase::Ready) {
            view.invitation = run.transport->invitation();
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
    return m_impl && (m_impl->play.phase() == OnlinePlay::Phase::Loading ||
                      m_impl->play.phase() == OnlinePlay::Phase::Playing ||
                      m_impl->play.phase() == OnlinePlay::Phase::Paused);
#else
    return false;
#endif
}
bool OnlineRun::pause(s32 device) {
#if GDL_ENABLE_NETPLAY
    return m_impl && m_impl->play.pause(device);
#else
    (void)device;
    return false;
#endif
}
void OnlineRun::resume() {
#if GDL_ENABLE_NETPLAY
    if (m_impl) {
        m_impl->play.resume();
    }
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
    if (run.play.phase() == OnlinePlay::Phase::Closed) {
        session.update(1.0 / 60);
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
                    status(*run.device,
                           run.selected ? "Ready when you are. Online progress is not saved yet."
                                        : "Select your characters, then mark yourself ready.");
                } else {
                    status(*run.device, std::format("Test room {} - {} machine(s). {}", room->code,
                                                    room->members.size(),
                                                    session.host() ? "Press Start to begin."
                                                                   : "Waiting for host."));
                }
                if (!run.interactive && session.host() && room->members.size() > 1 &&
                    (run.autoStart || menu.start || menu.select)) {
                    session.start();
                }
            }
        } else if (session.phase() == OnlineSession::Phase::Active) {
            const auto rules = session.room()->settings;
            run.config.difficulty.level = DifficultyConfig::kNames[rules.difficulty];
            run.config.multiplayer.mode = static_cast<MultiplayerMode>(rules.friendlyFire);
            if (!run.play.open(*run.device, run.context, session, run.selection, run.initial,
                               run.options)) {
                status(*run.device, "Online scene setup failed. Escape exits.");
                session.leave();
            } else {
                status(*run.device, "Loading the shared stage...");
            }
        }
    } else {
        if (!run.interactive && menu.start) {
            if (run.play.phase() == OnlinePlay::Phase::Paused) {
                run.play.resume();
            } else {
                run.play.pause();
            }
        }
        const auto phase = run.play.update(devices);
        switch (phase) {
        case OnlinePlay::Phase::Loading:
            status(*run.device, "Loading the shared stage...");
            run.reportedCheckpoint = false;
            break;
        case OnlinePlay::Phase::Playing:
            if (const auto* shown = run.play.shown();
                shown != nullptr && (shown->motion.epoch != run.reportedEpoch ||
                                     shown->motion.tick / 300 > run.reportedTick / 300)) {
                // Test-mode telemetry proves that both simulation and guest
                // presentation continue advancing after a loading barrier.
                run.reportedEpoch = shown->motion.epoch;
                run.reportedTick = shown->motion.tick;
                log::info("Online checkpoint progress: epoch {} tick {} ({})", run.reportedEpoch,
                          run.reportedTick, session.host() ? "host" : "guest");
            }
            if (session.host() && !run.reportedCheckpoint && run.play.shown() != nullptr) {
                if (const auto packets = CombatReplica::packets(
                        *run.play.shown(), SnapshotBlock::Compression::Automatic,
                        CombatReplica::Recovery::SingleLoss)) {
                    usize bytes = 0;
                    for (const auto& packet : *packets) {
                        bytes += packet.size();
                    }
                    log::info("Online initial checkpoint: {} datagrams, {} bytes including repair",
                              packets->size(), bytes);
                    run.reportedCheckpoint = true;
                }
            }
            status(*run.device, run.interactive ? ""
                                                : "Online scene test - Start opens a local menu; "
                                                  "Escape exits. No saves are written.");
            break;
        case OnlinePlay::Phase::Paused:
            status(*run.device, "Local menu open - the game continues for everyone.");
            break;
        case OnlinePlay::Phase::Finished:
            // Do not fall through into local post-level menus or reset guest progress.
            status(
                *run.device,
                "Stage finished. Online results/shop travel is not connected yet. Escape exits.");
            session.leave();
            run.finished = true;
            break;
        case OnlinePlay::Phase::Failed:
            status(*run.device, std::format("Online scene stopped ({}). Escape exits.",
                                            static_cast<u32>(run.play.failure())));
            run.finished = true;
            break;
        default: break;
        }
    }
    if (!run.finished && session.phase() == OnlineSession::Phase::Failed) {
        status(*run.device, std::format("Connection failed ({}). Escape exits.",
                                        static_cast<u32>(session.failure())));
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
        if (const auto* scene = run.play.hostScene()) {
            if (const auto* actor = scene->actor(seat)) {
                return scene->cursorAim(cursor, actor->position());
            }
        } else if (const auto* view = run.play.replica()) {
            return view->cursorAim(seat, cursor);
        }
    }
#else
    (void)device;
    (void)cursor;
#endif
    return std::nullopt;
}
void OnlineRun::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height,
                       f64 seconds, f32 frameBlend) {
#if GDL_ENABLE_NETPLAY
    if (m_impl) {
        m_impl->play.render(device, projection, width, height, seconds, frameBlend);
        if (m_impl->play.entryVisible()) {
            return;
        }
    }
#else
    (void)seconds;
    (void)frameBlend;
#endif
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
