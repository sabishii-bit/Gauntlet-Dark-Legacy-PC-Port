#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCamera.h"

#include "game/characters/Party.h"
#include "game/hud/LinkIndicator.h"
#include "game/online/NetplayMenu.h"
#include "game/online/OnlineLinks.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/SceneSelection.h"

namespace gdl::game {
struct GameServices;
struct LevelRef;
struct PlayOptions;
class PlayScene;
class LevelWorld;
/** Online runtime shared by the menu and development launcher. Transport/admission are
 * independent of presentation; selected local characters remain owned by their machine.
 * Completed shops return authenticated local checkpoints to the application. */
class OnlineRun {
public:
    static bool available();
    OnlineRun();
    ~OnlineRun();
    OnlineRun(const OnlineRun&) = delete;
    OnlineRun& operator=(const OnlineRun&) = delete;
    OnlineRun(OnlineRun&&) = delete;
    OnlineRun& operator=(OnlineRun&&) = delete;
    /** The run holds its links and hands them to the online game it opens. */
    using Links = OnlineLinks;
    bool open(RenderDevice& device, const GameServices& services, const Links& links,
              const std::string& endpoint, const std::string& code, const std::string& build,
              const std::string& content, std::span<const PartyMember> local,
              const LevelRef& initial, const PlayOptions& options, bool autoStart = false,
              const std::filesystem::path& invitationOutput = {});
    void update(const SessionInputs::Frame& devices, const MenuInput& menu);
    bool openLobby(RenderDevice& device, const GameServices& services, const Links& links,
                   const std::string& invitation, const std::string& build,
                   const std::string& content, u8 localPlayers, bool localOnly = false);
    bool select(std::span<const PartyMember> local);
    bool ready(bool value);
    bool start();
    /** The host's: sets the room's rules; false for a mechanic this build does not offer. */
    bool settings(const RoomSettings& value);
    NetplayMenu::View lobby() const;
    bool playing() const;
    u64 simulatedTick() const;
    u64 confirmedTick() const;
    /** Links straight to other guests, bypassing the host for their inputs. */
    usize directLinks() const;
    bool pause(s32 device);
    void resume();
    bool hasCompletedProgress() const;
    bool saveCompletedProgress();
    std::optional<Vec3> cursorAim(s32 device, Vec2 cursor) const;
    bool consoleHost() const;
    bool consoleAllowed() const;
    bool setConsolePermissions(bool enabled);
    bool consoleCommand(u8 seat, std::string line);
    const PlayScene* consoleScene() const;
    const LevelWorld* consoleWorld() const;
    std::optional<u8> consoleSeat(s32 device) const;
    void consoleCapture(bool captured);
    void consoleView(bool hideHud, const std::optional<WorldCamera>& camera);
    void consoleSelect(const std::optional<SceneSelection>& selection);
    std::optional<SceneSelection> consolePick(Vec2 cursor);
    std::vector<std::string> takeConsoleReplies();
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height, f64 seconds,
                f32 frameBlend);
    void close();
    const std::string& status() const { return m_status; }
    /** The line drawn in the corner while playing: the ping alone; the rollback's other
     * figures go to the log. Unknown ping reads as such. */
    static std::string networkSummary(s32 pingMs);
    const std::string& network() const { return m_network; }
    /** How long the quietest peer may send nothing, starving the simulation, before the host
     * drops it and play goes on; a guest leaves instead when it is the host that went quiet. */
    static constexpr f64 kStallDeadlineSeconds = 15;
    /** Seconds a stall may last before the players are told about it. */
    static constexpr f64 kStallNoticeSeconds = 1;
    /** The status shown `stalledSeconds` into a stall: nothing at first, then the wait
     * and the seconds left before the deadline. */
    static std::optional<std::string> stallNotice(f64 stalledSeconds);

private:
    bool connect(RenderDevice& device, const GameServices& services, const Links& links,
                 bool localOnly, const std::string& invitation, const std::string& build,
                 const std::string& content, u8 localPlayers);
    void status(RenderDevice& device, std::string message);
    void network(RenderDevice& device, std::string line, s32 pingMs);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::unique_ptr<Texture> m_statusTexture;
    std::string m_status;
    std::unique_ptr<Texture> m_networkTexture;
    std::string m_network;
    s32 m_pingMs = -1;
};
} // namespace gdl::game
