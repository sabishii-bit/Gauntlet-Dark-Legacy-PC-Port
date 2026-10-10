#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>

#include "engine/render/RenderDevice.h"

#include "game/players/Party.h"
#include "game/screens/SessionInputs.h"

namespace gdl::game {
struct GameContext;
struct LevelRef;
struct PlayOptions;
/** Playable loopback integration run, available only in a netplay-enabled build.
 * A launcher supplies a freshly computed asset digest and scenario selections;
 * no saves are opened or written. Public lobby/post-level UI is separate work. */
class OnlineRun {
public:
    OnlineRun();
    ~OnlineRun();
    OnlineRun(const OnlineRun&) = delete;
    OnlineRun& operator=(const OnlineRun&) = delete;
    OnlineRun(OnlineRun&&) = delete;
    OnlineRun& operator=(OnlineRun&&) = delete;
    bool open(RenderDevice& device, const GameContext& context, const std::string& endpoint,
              const std::string& code, const std::string& build, const std::string& content,
              std::span<const PartyMember> local, const LevelRef& initial,
              const PlayOptions& options, bool autoStart = false);
    void update(const SessionInputs::Frame& devices, const MenuInput& menu);
    std::optional<Vec3> cursorAim(s32 device, Vec2 cursor) const;
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height, f64 seconds,
                f32 frameBlend);
    void close();
    const std::string& status() const { return m_status; }

private:
    void status(RenderDevice& device, std::string message);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::unique_ptr<Texture> m_statusTexture;
    std::string m_status;
};
} // namespace gdl::game
