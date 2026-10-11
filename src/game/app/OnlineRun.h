#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "engine/render/RenderDevice.h"

#include "game/players/Party.h"
#include "game/screens/NetplayMenu.h"
#include "game/screens/SessionInputs.h"

namespace gdl::game {
struct GameContext;
struct LevelRef;
struct PlayOptions;
/** Online runtime shared by the menu and development launcher. Transport/admission are
 * independent of presentation; selected local characters remain owned by their machine.
 * No saves are written here; online results/shop persistence is separate work. */
class OnlineRun {
public:
    static bool available();
    OnlineRun();
    ~OnlineRun();
    OnlineRun(const OnlineRun&) = delete;
    OnlineRun& operator=(const OnlineRun&) = delete;
    OnlineRun(OnlineRun&&) = delete;
    OnlineRun& operator=(OnlineRun&&) = delete;
    bool open(RenderDevice& device, const GameContext& context, const std::string& endpoint,
              const std::string& code, const std::string& build, const std::string& content,
              std::span<const PartyMember> local, const LevelRef& initial,
              const PlayOptions& options, bool autoStart = false,
              const std::filesystem::path& invitationOutput = {});
    void update(const SessionInputs::Frame& devices, const MenuInput& menu);
    bool openLobby(RenderDevice& device, const GameContext& context, const std::string& invitation,
                   const std::string& build, const std::string& content, u8 localPlayers,
                   bool localOnly = false);
    bool select(std::span<const PartyMember> local);
    bool ready(bool value);
    bool start();
    bool settings(const RoomSettings& value);
    NetplayMenu::View lobby() const;
    bool playing() const;
    bool pause(s32 device);
    void resume();
    std::optional<Vec3> cursorAim(s32 device, Vec2 cursor) const;
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height, f64 seconds,
                f32 frameBlend);
    void close();
    const std::string& status() const { return m_status; }

private:
    bool connect(RenderDevice& device, const GameContext& context, bool localOnly,
                 const std::string& invitation, const std::string& build,
                 const std::string& content, u8 localPlayers);
    void status(RenderDevice& device, std::string message);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::unique_ptr<Texture> m_statusTexture;
    std::string m_status;
};
} // namespace gdl::game
