#pragma once

#include "game/netplay/ClientClock.h"
#include "game/screens/OnlineLevelEntry.h"
#include "game/screens/OnlineParty.h"
#include "game/screens/PlayReplication.h"
#include "game/screens/ReplicaStage.h"

namespace gdl::game {
/** Scene owner for an admitted online party. The host alone constructs a
 * PlayScene; a guest owns only native artwork and checkpoint presentation.
 * update() pumps the session once per 60 Hz application tick, including pause.
 * render() advances only presentation. No save I/O or results/shop decisions.
 * The session, selection, device and context dependencies must outlive us. */
class OnlinePlay {
public:
    enum class Phase : u8 { Closed, Loading, Playing, Paused, Finished, Failed };
    enum class Failure : u8 {
        None,
        Session,
        Scene,
        Assets,
        Input,
        Capture,
        Presentation,
        UnsupportedTravel,
        Party
    };
    OnlinePlay();
    ~OnlinePlay();
    OnlinePlay(const OnlinePlay&) = delete;
    OnlinePlay& operator=(const OnlinePlay&) = delete;
    OnlinePlay(OnlinePlay&&) = delete;
    OnlinePlay& operator=(OnlinePlay&&) = delete;

    /** Initial scene: host supplies its locally resolved destination/options;
     * guests ignore those arguments and wait for the authenticated host scene ID.
     * Selection is frozen by OnlineSession before this call. */
    bool open(RenderDevice& device, const GameContext& context, OnlineSession& session,
              const OnlineParty& selection, const LevelRef& initial = LevelRef::tower(),
              const PlayOptions& options = {});
    void close();
    Phase update(const SessionInputs::Frame& devices);
    bool pause();
    bool resume(); // host authority only
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height, f64 seconds,
                f32 frameBlend);
    Phase phase() const { return m_phase; }
    Failure failure() const { return m_failure; }
    PlayReplication::Result outcome() const { return m_outcome; }
    const PlayScene* hostScene() const;
    const ReplicaView* replica() const { return m_replica.view(); }
    const CombatSnapshot* shown() const;
    OnlineLevelEntry::Phase entryPhase() const { return m_entry.phase(); }
    bool entryVisible() const { return m_entry.visible(); }

    /** Stable native realm/index encoding, not directory-enumeration order.
     * A packet can select only a playable entry in the trusted local catalog. */
    static std::optional<u32> sceneId(const LevelCatalog& catalog, const LevelRef& level);
    static std::optional<LevelRef> levelOf(const LevelCatalog& catalog, u32 scene);

private:
    struct Host;
    bool load(const SessionInputs::Frame& devices);
    bool travel();
    Phase fail(Failure reason);
    void quiet();
    RenderDevice* m_device = nullptr;
    GameContext m_context;
    OnlineSession* m_session = nullptr;
    const OnlineParty* m_selection = nullptr;
    PlayOptions m_options;
    std::vector<PartyMember> m_travelMembers;
    std::unique_ptr<Host> m_host;
    ReplicaStage m_replica;
    ClientClock m_clock;
    OnlineLevelEntry m_entry;
    u64 m_entryEpoch = 0;
    Phase m_phase = Phase::Closed;
    Failure m_failure = Failure::None;
    PlayReplication::Result m_outcome = PlayReplication::Result::Held;
    u64 m_epoch = 0;
};
} // namespace gdl::game
