#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

#include "expansions/n64/camera/LegendsCameraChannel.h"
#include "expansions/n64/camera/LegendsCameraRig.h"
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
        /** The stones make all thirteen (e086af00): its script returns as it settles, and
         * the rune portal waits for that. */
        bool completesRunes = false;
        /** The shards make the window (e086ae04): its script turns to the window's reveal. */
        bool completesWindow = false;
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
     * save never manufactures a newly acquired piece. Setup e08694e4 selects one new
     * piece of each kind for the dispatcher, which runs each selected script once. */
    void begin(const LegendsProgress& before, const LegendsProgress& after);
    bool pending() const { return m_next < m_entries.size(); }
    bool active() const { return m_started; }
    /** Dispatcher case 0 with its update's gesture pass: e007de28 settles the camera channels on
     * the gameplay `camera`, e0860620 cuts them to Sumner's shot about the midpoint of him and
     * `party`, the standing party's mean feet (e086007c), and REDY starts. The cut shows once a
     * script steps the camera; until then the view stays `camera`, looking at `attention`,
     * where the return glides back to. The next tick selects the first entry. */
    void start(const WorldCamera& camera, const Vec3& attention, const Vec3& party);
    void update(f32 seconds);
    std::span<const Entry> entries() const { return m_entries; }
    const Entry* current() const;
    /** The party's pieces as this visit draws them: a selected piece shows once its script
     * places it, and the other new stones and panes stay undrawn until the next visit
     * (e0861630 draws the seen stones, e0862fc4 hides the new panes). e0864e7c builds the
     * doors it did not select already open. */
    LegendsProgress displayed(const LegendsProgress& party) const;
    /** The party's progress as the completed window and the rune portal show it: a
     * completion this visit brings waits for its reveal, the window's turn (e086ae04) or
     * the thirteenth stone's settling (e086aec0). */
    LegendsProgress completed(const LegendsProgress& party) const;
    /** displayed() for the party begin() was given. */
    LegendsProgress displayedProgress() const { return displayed(m_owned); }
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
    /** CATH_BEAM's hidden flag is clear: from the setup when the seen shards make the window
     * (e0862fc4), else from the window's case 7. */
    bool cathedralBeamShown() const { return m_beamShown; }
    /** The prim alpha e001ea34 gives CATH_BEAM: 255 less its native transparency byte, over
     * 255, and 0 while it is hidden. */
    f32 cathedralBeamAlpha() const;
    /** The caption on screen; a queued one shows from the next caption update. */
    std::optional<u32> caption() const;
    f32 captionOpacity() const;
    std::vector<u32> takeSounds();
    /** The view the camera channels last built (e007dd70), from the previous update's with a
     * blend `alpha`; the gameplay camera until the cut shows. */
    WorldCamera camera(f32 alpha = -1) const;
    /** The point that view looks at. While the return has the attention channel off (e007df20),
     * a glide from the last shot's point to the gameplay attention on the return's times. */
    const Vec3& attention() const { return m_cameraFocus; }
    const Vec3& sumnerPosition() const { return m_native.sumner.position; }
    /** The yaw dispatcher case 0's cut (e0860620) turns an arrived player standing at
     * `position` to through game.bin e0048acc: e0010e50's arc tangent of Sumner's x and z
     * offsets from it, in the native yaw the N64 actors keep. Sumner keeps his own yaw. */
    f32 partyFacing(const Vec3& position) const;

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
    /** e007d53c and e007d584 as the scripts call them: both move on the shots' times. */
    void shot(f32 distance, f32 pitch, f32 yaw, const Vec3& focus);
    void planReturn();
    /** A script's e007dd70: true once every camera channel rests. */
    bool stepCamera(f32 seconds);
    void makeRuneFlight(usize index);
    void queueCaptions(std::span<const u32> ids);
    formats::LegendsN64Tower m_native;
    std::array<u16, 13> m_runeCaptions{};
    std::array<std::array<u16, 2>, 4> m_shardCaptions{};
    std::array<std::array<u16, 2>, 4> m_doorCaptions{};
    std::array<Vec3, 4> m_doorPoints{}; ///< e0869634's rows, by door
    std::array<f32, 4> m_doorYaws{};    ///< e08696c4's rows in radians, by door
    std::array<u16, 2> m_windowCaptions{};
    Vec3 m_runePoint{0};  ///< e086967c
    Vec3 m_shardPoint{0}; ///< e0869670
    std::vector<Sequence> m_sequences;
    std::vector<Entry> m_entries;
    LegendsProgress m_owned;      ///< begin()'s party with the pieces seen before it
    LegendsProgress m_withheld;   ///< pieces this visit does not draw (yet)
    LegendsProgress m_unrevealed; ///< pieces whose completion waits for its reveal
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
    LegendsCameraRig m_rig;
    WorldCamera m_camera; ///< the view shown: the rig's last build, or the gameplay camera
    WorldCamera m_previousCamera;
    bool m_cutShown = false; ///< a step has built the view since start()
    WorldCamera m_returnCamera;
    Vec3 m_party{0}; ///< e086007c's mean, which case 0x28 finds again with the party held
    Vec3 m_cameraFocus{0};
    Vec3 m_returnFocus{0};
    LegendsCameraChannel m_reportedFocus; ///< attention()'s glide while the return is under way
};

} // namespace gdl::game
