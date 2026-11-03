#pragma once

#include <memory>
#include <string_view>

#include "game/online/NativePlaySetup.h"
#include "game/online/NativeRollbackDriver.h"
#include "game/online/OnlineCompletion.h"
#include "game/online/OnlineLevelEntry.h"
#include "game/online/OnlineLinks.h"

namespace gdl::game {
class AfterLevelScene;
class OnlineParty;
class OnlineSession;

/** Campaign owner for an agreed native-input session. Every peer owns and renders
 * the actual PlayScene, predicting missing remote controls with bounded native
 * rewind. update() pumps transport once per 60 Hz application tick, even when
 * inputs are held, a local menu is open, or a terminal scene awaits departure.
 * The device, session, selection and the services it copies its view from must outlive
 * us. */
class NativeOnlinePlay {
public:
    enum class Phase : u8 {
        Closed,
        Loading,
        Playing,
        Paused,
        Shopping,
        Resyncing, ///< this peer waits for, or replays, the host's journal
        Finished,
        Failed
    };
    enum class Failure : u8 { None, Session, Scene, Assets, Input, Presentation, Party };
    /** Shop steps a guest may hold and still hear each one's menu cues: a burst of a tenth of
     * a second sounds, while a longer backlog plays only its newest steps' cues. */
    static constexpr usize kHeardShopSteps = 6;
    /** Whether a shop step's menu cues are heard, `held` steps still behind it: always on the
     * host, which steps the shop itself. */
    static bool shopCuesHeard(bool host, usize held) { return host || held < kHeardShopSteps; }

    NativeOnlinePlay();
    ~NativeOnlinePlay();
    NativeOnlinePlay(const NativeOnlinePlay&) = delete;
    NativeOnlinePlay& operator=(const NativeOnlinePlay&) = delete;
    NativeOnlinePlay(NativeOnlinePlay&&) = delete;
    NativeOnlinePlay& operator=(NativeOnlinePlay&&) = delete;

    /** The game lets its links go as it closes. */
    using Links = OnlineLinks;

    /** setup is the same owner whose digest the session already agreed before
     * becoming Active. It remains alive across ordinary travel and secret-parent
     * suspension. This method does not replace the setup agreement handshake. */
    bool open(RenderDevice& device, const GameServices& services, OnlineSession& session,
              OnlineParty& selection, std::unique_ptr<NativePlaySetup> setup, const Links& links);
    void close();
    Phase update(const SessionInputs::Frame& devices);
    /** Captures only the specified local device's controls; never pauses the
     * shared scene or its clocks. -1 captures/releases all local devices. */
    bool pause(s32 device = -1);
    bool resume(s32 device = -1);
    /** Only completed shop/secret checkpoints, never the current live scene. */
    bool hasCompletedProgress() const;
    bool saveCompletedProgress();
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height, f64 seconds,
                f32 frameBlend);

    Phase phase() const;
    Failure failure() const;
    /** Retained after teardown; contains no room invitation or peer identifier. */
    std::string_view failureDetail() const;
    NativeRollbackDriver::Status outcome() const;
    const PlayScene* scene() const;
    const LevelWorld* world() const;
    /** Local presentation/input capture only. Never changes the shared scene's clock. */
    void consoleCapture(bool captured);
    void consoleView(bool hideHud, const std::optional<WorldCamera>& camera);
    void consoleSelect(const std::optional<SceneSelection>& selection);
    std::optional<SceneSelection> consolePick(Vec2 cursor);
    std::vector<std::string> takeConsoleReplies();
    /** The displayed state can be ahead of the all-seat confirmed frontier. */
    u64 simulatedTick() const;
    u64 confirmedTick() const;
    NativeRollbackDriver::Statistics rollbackStatistics() const;
    OnlineLevelEntry::Phase entryPhase() const;
    bool entryVisible() const;
    const OnlineCompletion& completion() const;
    const AfterLevelScene* shopScene() const;

    static std::optional<u32> sceneId(const LevelCatalog& catalog, const LevelRef& level);
    static std::optional<LevelRef> levelOf(const LevelCatalog& catalog, u32 scene);

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
} // namespace gdl::game
