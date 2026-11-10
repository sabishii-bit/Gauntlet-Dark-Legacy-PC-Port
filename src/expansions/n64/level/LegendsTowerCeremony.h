#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/world/WorldCamera.h"

#include "expansions/n64/formats/LegendsN64Tower.h"
#include "expansions/shared/characters/LegendsProgress.h"

namespace gdl::game {

/** Local Tower presentation, independent of ownership/save transactions. Native
 * e086736c dispatches rune, door and shard scripts while gameplay is held. */
class LegendsTowerCeremony {
public:
    enum class Kind { Rune, Door, Shard };
    struct Entry {
        Kind kind = Kind::Rune;
        usize index = 0;
        std::vector<u32> captions;
    };
    struct Sequence {
        std::string name;
        s32 frames = 0;
        f32 secondsPerFrame = 1.0f / 30;
    };
    static LegendsTowerCeremony decode(std::span<const u8> overlay);
    void bindSequences(std::vector<Sequence> sequences);
    /** Only the completed travel path supplies before/after. Loading an existing
     * save never manufactures a newly acquired piece. */
    void begin(const LegendsProgress& before, const LegendsProgress& after);
    bool pending() const { return m_next < m_entries.size(); }
    bool active() const { return m_started; }
    void start(const WorldCamera& camera, const Vec3& attention);
    void update(f32 seconds);
    std::span<const Entry> entries() const { return m_entries; }
    const Entry* current() const;
    const LegendsProgress& displayedProgress() const { return m_displayed; }
    std::string_view gesture() const { return m_gesture; }
    f32 gestureFrame() const { return m_frame; }
    bool staffVisible() const { return m_started; }
    bool runeInFlight() const { return m_flightFrame >= 0; }
    Mat4 runePlacement() const;
    f32 runeOpacity() const;
    f32 shardOpacity() const;
    f32 shardWhiteBlend() const;
    f32 windowReveal() const { return m_windowReveal; }
    std::optional<u32> caption() const;
    f32 captionOpacity() const;
    std::vector<u32> takeSounds();
    WorldCamera camera(f32 alpha = -1) const;
    const Vec3& attention() const { return m_cameraFocus; }
    const Vec3& sumnerPosition() const { return m_native.sumner.position; }

private:
    void tick();
    void next();
    void finishEntry();
    void play(std::string_view name, bool reverse = false, f32 speed = 1);
    bool advanceGesture();
    void shot(const Vec3& focus, f32 distance, f32 pitch);
    void updateCamera();
    void makeRuneFlight(usize index);
    void queueCaptions(std::span<const u32> ids);
    formats::LegendsN64Tower m_native;
    std::array<u16, 13> m_runeCaptions{};
    std::array<std::array<u16, 2>, 4> m_shardCaptions{};
    std::array<std::array<u16, 2>, 4> m_doorCaptions{};
    std::array<Vec3, 4> m_doorFocus{};
    std::array<u16, 2> m_windowCaptions{};
    Vec3 m_runeFocus{0};
    Vec3 m_shardFocus{0};
    std::vector<Sequence> m_sequences;
    std::vector<Entry> m_entries;
    LegendsProgress m_displayed;
    LegendsProgress m_owned;
    usize m_next = 0;
    s32 m_phase = 0;
    s32 m_ticks = 0;
    bool m_started = false;
    f64 m_remainder = 0;
    std::string m_gesture;
    f32 m_frame = 0;
    f32 m_frameStep = 1;
    f32 m_frameEnd = 0;
    s32 m_flightFrame = -1;
    struct FlightPose {
        Vec3 position{0};
        Vec3 angles{0};
    };
    std::array<FlightPose, 62> m_flight{};
    f32 m_windowReveal = 0;
    std::vector<u32> m_sounds;
    std::vector<u32> m_captions;
    f64 m_captionSeconds = 0;
    WorldCamera m_camera;
    WorldCamera m_previousCamera;
    WorldCamera m_returnCamera;
    WorldCamera m_cameraFrom;
    WorldCamera m_cameraGoal;
    Vec3 m_cameraFocus{0};
    Vec3 m_focusFrom{0};
    Vec3 m_focusGoal{0};
    Vec3 m_returnFocus{0};
    f32 m_cameraSeconds = 0;
};

} // namespace gdl::game
