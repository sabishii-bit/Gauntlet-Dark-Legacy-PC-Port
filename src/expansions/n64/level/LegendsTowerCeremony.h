#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

#include "expansions/n64/formats/LegendsN64Tower.h"
#include "expansions/shared/characters/LegendsProgress.h"

namespace gdl::game {

/** Local Tower presentation, independent of ownership/save transactions. Native
 * e086736c dispatches rune, door and shard scripts while gameplay is held. Each
 * 30 Hz tick runs one dispatcher case, then e0860d1c's ready restart and caption
 * clock on script and return ticks, then the gesture pass (e0032c80), so a script
 * reads the frame and end flag that the previous tick's pass left. */
class LegendsTowerCeremony {
public:
    enum class Kind { Rune, Door, Shard };
    struct Entry {
        Kind kind = Kind::Rune;
        usize index = 0;
        std::vector<u32> captions;
        /** The stone completes all thirteen (e086af00): its script returns as it settles. */
        bool completesRunes = false;
    };
    struct Sequence {
        std::string name;
        s32 frames = 0;
        f32 secondsPerFrame = 1.0f / 30;
        bool repeats = false; ///< wraps at its end (WIZMOVES REDY); the others play once
    };
    static LegendsTowerCeremony decode(std::span<const u8> overlay);
    void bindSequences(std::vector<Sequence> sequences);
    /** Sumner's gestures as bindSequences received them. */
    std::span<const Sequence> sequences() const { return m_sequences; }
    /** Only the completed travel path supplies before/after. Loading an existing
     * save never manufactures a newly acquired piece. */
    void begin(const LegendsProgress& before, const LegendsProgress& after);
    bool pending() const { return m_next < m_entries.size(); }
    bool active() const { return m_started; }
    /** Dispatcher case 0 (e0860620) with its update's gesture pass: cuts to Sumner's
     * shot and starts REDY. The next tick selects the first entry. */
    void start(const WorldCamera& camera, const Vec3& attention);
    void update(f32 seconds);
    std::span<const Entry> entries() const { return m_entries; }
    const Entry* current() const;
    const LegendsProgress& displayedProgress() const { return m_displayed; }
    std::string_view gesture() const { return m_gesture.name; }
    /** The frame Sumner shows: a reversed sequence counts back from its end. */
    f32 gestureFrame() const;
    bool staffVisible() const { return m_started; }
    bool runeInFlight() const { return m_flightFrame >= 0; }
    Mat4 runePlacement() const;
    f32 runeOpacity() const;
    f32 shardOpacity() const;
    f32 shardWhiteBlend() const;
    f32 windowReveal() const { return m_windowReveal; }
    /** CATH_BEAM's opacity from its native transparency byte, 0 while it is hidden. */
    f32 cathedralBeamOpacity() const;
    /** The caption on screen; a queued one shows from the next caption update. */
    std::optional<u32> caption() const;
    f32 captionOpacity() const;
    std::vector<u32> takeSounds();
    WorldCamera camera(f32 alpha = -1) const;
    const Vec3& attention() const { return m_cameraFocus; }
    const Vec3& sumnerPosition() const { return m_native.sumner.position; }

private:
    /** e086736c's case for the next tick. */
    enum class Dispatch { Select, Script, PlanReturn, Return, HandBack };
    /** e086239c's stage counter (e086ac78). */
    enum class RuneStage { Waiting, Spell, Ready };
    /** e0865c98's cases (table e086a9d8); STRT plays reversed, then forward. */
    enum class DoorCase {
        Captions,
        Opening,
        Shot,
        Spell,
        SpellReversed,
        SpellForward,
        Bolt,
        Release,
        Closing,
        Finished
    };
    /** e0863440's cases (table e086a7f0); Closing and Finished are 0x5a and 0x5b. */
    enum class ShardCase {
        Captions,
        Opening,
        Shot,
        Spell,
        Install,
        Rest,
        Settle,
        Restore,
        WindowWait,
        Reveal,
        Revealed,
        Closing,
        Finished
    };
    /** Sumner's animation object as e002f7a0 starts it and e00327e4 passes it. */
    struct Gesture {
        std::string name;
        s32 frames = 0;
        f32 step = 1;          ///< frames a pass adds (16 / the header's rate)
        f32 frame = 0;         ///< +0x2c
        f32 previousFrame = 0; ///< +0x30, the frame before the last pass
        bool repeats = false;  ///< the sequence's header wraps it
        bool once = false;     ///< 0x20000: the Tower plays it once whatever its header says
        bool reversed = false; ///< 4: shown from its last frame back
        bool held = false;     ///< 0x10000000: the next pass only records the frame
        bool done = false;     ///< 0x1000: a pass reached the end
        bool frozen = false;   ///< 0x8000: passes skip it
    };
    /** One camera move as e007cdf0 plans it and e007d774 advances it: accelerate,
     * cruise and brake, each phase on its own clock, as a fraction of the move. */
    struct Glide {
        enum class Phase { Accelerating, Cruising, Braking, Done };
        Phase phase = Phase::Done;
        f32 acceleration = 0; ///< seconds to reach the cruising speed
        f32 elapsed = 0;      ///< seconds into the phase
        f32 speed = 0;
        f32 cruiseSpeed = 0;
        f32 rise = 0;      ///< speed gained a second while accelerating
        f32 fall = 0;      ///< speed lost a second while braking
        f32 travelled = 0; ///< fraction of the move made

        void plan(f32 accelerationSeconds);
        /** True once the braking phase has run out. */
        bool advance(f32 seconds);
    };
    /** e08608e8's seconds a caption stays, and a drawable's clear transparency byte. */
    static constexpr f32 kCaptionSeconds = 3;
    static constexpr u8 kTransparent = 255;
    /** A queued caption as e08608e8 stores it and e086098c shows it. */
    struct Caption {
        enum class Stage : u8 { Queued, FadingIn, Shown, FadingOut };
        u32 id = 0;
        f32 remaining = kCaptionSeconds;
        Stage stage = Stage::Queued;
        u8 transparency = kTransparent;
    };
    /** The pane's drawable transparency and white bytes (+1, +5). */
    struct PaneBytes {
        u8 transparency = 0;
        u8 white = 0;
    };
    struct FlightPose {
        Vec3 position{0};
        Vec3 angles{0};
    };
    /** e08618a8 records 1 + 4 + 4 + 3 + 50 poses (e086aea4). */
    static constexpr usize kFlightPoses = 62;

    static PaneBytes paneBytes(s32 count);
    void tick();
    void select();
    bool runScript(const Entry& entry);
    bool runeScript(const Entry& entry);
    bool doorScript(const Entry& entry);
    bool shardScript(const Entry& entry);
    void attend();
    void stepCaptions();
    void play(std::string_view name);
    void perform(std::string_view name);
    void passGesture();
    bool crossed(f32 frame) const;
    void shot(const Vec3& focus, f32 distance, f32 pitch);
    void glideTo(const WorldCamera& goal, const Vec3& focus, f32 acceleration);
    bool advanceCamera();
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
    bool m_started = false;
    f64 m_remainder = 0;
    Dispatch m_dispatch = Dispatch::Select;
    s32 m_count = 0; ///< the door's or shard's update count (e086ac94, e086ac80)
    DoorCase m_doorCase = DoorCase::Captions;
    s32 m_doorHold = -1;     ///< e086ac98: ODOR's hold countdown, negative when not holding
    bool m_doorBolt = false; ///< e086ae1c: the bolt from the staff to the door is out
    ShardCase m_shardCase = ShardCase::Captions;
    s32 m_runeTime = 0; ///< e086ac7c in updates: dt * 30 is one at the 30 Hz tick
    RuneStage m_runeStage = RuneStage::Waiting;
    s32 m_spellAt = -1;             ///< slot 2: the update MKRU starts
    s32 m_flightStart = -1;         ///< slot 1: the update the flight shows its first pose
    s32 m_settledAt = -1;           ///< slot 3, or slot 0 for the thirteenth stone
    bool m_readySuppressed = false; ///< e086ada0: no ready restart while the shard spell runs
    Gesture m_gesture;
    s32 m_flightFrame = -1;
    std::array<FlightPose, kFlightPoses> m_flight{};
    std::optional<PaneBytes> m_paneBytes; ///< from the install until the pane settles
    f32 m_windowReveal = 0;
    bool m_beamShown = false;
    u8 m_beamTransparency = 0;
    std::vector<u32> m_sounds;
    std::vector<Caption> m_captions;
    WorldCamera m_camera;
    WorldCamera m_previousCamera;
    WorldCamera m_returnCamera;
    WorldCamera m_cameraFrom;
    WorldCamera m_cameraGoal;
    Vec3 m_cameraFocus{0};
    Vec3 m_focusFrom{0};
    Vec3 m_focusGoal{0};
    Vec3 m_returnFocus{0};
    Glide m_glide;
};

} // namespace gdl::game
