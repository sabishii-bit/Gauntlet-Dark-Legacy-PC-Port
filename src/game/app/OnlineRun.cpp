#include "game/app/OnlineRun.h"

#include <algorithm>
#include <format>

#include "engine/core/Log.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/SystemFont.h"

#if GDL_ENABLE_NETPLAY
#include "engine/net/GnsTransport.h"

#include "game/netplay/RoomSession.h"
#include "game/screens/OnlinePlay.h"
#endif

namespace gdl::game {
struct OnlineRun::Impl {
#if GDL_ENABLE_NETPLAY
    RenderDevice* device = nullptr;
    GameContext context;
    LevelRef initial;
    PlayOptions options;
    std::unique_ptr<RoomSession> service;
    std::unique_ptr<GnsTransport> transport;
    std::unique_ptr<OnlineSession> session;
    OnlineParty selection;
    OnlinePlay play;
    bool autoStart = false;
    bool finished = false;
    bool reportedCheckpoint = false;
    u64 reportedEpoch = 0;
    u64 reportedTick = 0;
#endif
};
OnlineRun::OnlineRun() = default;
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
                     const PlayOptions& options, bool autoStart) {
    close();
#if GDL_ENABLE_NETPLAY
    if (local.empty() || local.size() > InputCommand::kSeats) {
        status(device, "Online test requires one to four local characters.");
        return false;
    }
    auto next = std::make_unique<Impl>();
    next->device = &device;
    next->context = context;
    next->initial = initial;
    next->options = options;
    next->autoStart = autoStart;
    std::string error;
    next->transport = GnsTransport::create({}, error);
    if (!next->transport) {
        status(device, "Network startup failed: " + error);
        return false;
    }
    next->service = std::make_unique<RoomSession>(endpoint, code, static_cast<u8>(local.size()),
                                                  build, content);
    next->session = std::make_unique<OnlineSession>(*next->service, *next->transport, code.empty(),
                                                    static_cast<u8>(local.size()));
    if (!next->selection.select(*next->session, local)) {
        status(device, "Online character selection was rejected.");
        return false;
    }
    m_impl = std::move(next);
    status(device, "Connecting to the test room...");
    return true;
#else
    (void)context;
    (void)endpoint;
    (void)code;
    (void)build;
    (void)content;
    (void)local;
    (void)initial;
    (void)options;
    (void)autoStart;
    status(device, "This build does not include the experimental netplay runtime.");
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
                if (own != room->members.end() && !own->ready) {
                    session.ready();
                }
                status(*run.device,
                       std::format("Test room {} - {} machine(s). {}", room->code,
                                   room->members.size(),
                                   session.host() ? "Press Start to begin." : "Waiting for host."));
                if (session.host() && room->members.size() > 1 &&
                    (run.autoStart || menu.start || menu.select)) {
                    session.start();
                }
            }
        } else if (session.phase() == OnlineSession::Phase::Active) {
            if (!run.play.open(*run.device, run.context, session, run.selection, run.initial,
                               run.options)) {
                status(*run.device, "Online scene setup failed. Escape exits.");
                session.leave();
            } else {
                status(*run.device, "Loading the shared stage...");
            }
        }
    } else {
        if (menu.start) {
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
            status(*run.device,
                   "Online scene test - Start pauses; Escape exits. No saves are written.");
            break;
        case OnlinePlay::Phase::Paused:
            status(*run.device, "Online test paused - the host can press Start to resume.");
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
