#include "expansions/n64/level/LegendsTowerCeremony.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <random>
#include <utility>

#include "engine/io/ByteReader.h"
#include "engine/math/PortableMath.h"

#include "expansions/n64/level/LegendsTowerProgress.h"

namespace gdl::game {
namespace {
constexpr f32 kTick = 1.0f / 30;
/// Caption slots the overlay's tables name whose lines are empty continuations.
constexpr std::array<u32, 3> kEmptyCaptions{521, 522, 526};
bool captionHasText(u32 id) {
    return id != 0 && std::ranges::find(kEmptyCaptions, id) == kEmptyCaptions.end();
}
std::vector<u32> captions(std::span<const u16> ids) {
    std::vector<u32> result;
    for (const auto id : ids) {
        if (captionHasText(id)) {
            result.push_back(id);
        }
    }
    return result;
}

// Sumner's WIZMOVES gestures.
constexpr std::string_view kReady = "REDY";
constexpr std::string_view kRuneSpell = "MKRU";
constexpr std::string_view kDoorSpell = "STRT";
constexpr std::string_view kDoorRelease = "ODOR";
constexpr std::string_view kShardSpell = "SHD1";
constexpr std::string_view kShardRestore = "SHDR";
/// The shortest frame period a bound sequence may claim.
constexpr f32 kMinimumPeriod = .0001f;

/** The bound gesture e002f708 would start: named and with frames to play. */
const LegendsTowerCeremony::Sequence*
findSequence(std::span<const LegendsTowerCeremony::Sequence> sequences, std::string_view name) {
    const auto found = std::ranges::find(sequences, name, &LegendsTowerCeremony::Sequence::name);
    return found != sequences.end() && found->frames > 0 ? &*found : nullptr;
}

// Shots: e007d53c's distance and pitch about e007d584's focus.
constexpr f32 kShotDistance = 30;
constexpr f32 kSumnerShotPitch = kPi / 6;
constexpr f32 kSumnerFocusHeight = 3;
constexpr f32 kRuneShotPitch = 25 * kPi / 180;
constexpr f32 kDoorShotPitch = 25 * kPi / 180;
constexpr f32 kShardShotPitch = 10 * kPi / 180;
constexpr f32 kWindowShotDistance = 38;
constexpr f32 kWindowShotPitch = kPi / 6;
/// The window shot looks 2 under the overlay's shard point, 7 under the shard shot's focus.
constexpr f32 kWindowFocusDrop = 7;
// Camera moves (e007cfd0): e08694e4 accelerates shots over .7 s, case 0x28 the return over .5 s.
constexpr f32 kShotAcceleration = .7f;
constexpr f32 kReturnAcceleration = .5f;
constexpr f32 kCruiseSeconds = 1;
constexpr f32 kBrakingSeconds = .3f;

/** A shot of focus from distance at pitch. It keeps the base camera's yaw rather than the
 * one e007d53c is given, until the native yaw convention is matched on screen. */
WorldCamera shotCamera(WorldCamera base, const Vec3& focus, f32 distance, f32 pitch) {
    base.pitch = pitch;
    base.roll = 0;
    base.position = focus - base.forward() * distance;
    return base;
}

// e086239c's time line, in updates from its setup.
constexpr s32 kRuneShotTime = 20;
constexpr s32 kRuneSpellTime = 50;
/// e086239c halves MKRU's speed (+0x14) as it starts it.
constexpr f32 kRuneSpellSpeed = .5f;
constexpr f32 kRuneReleaseFrame = 14;
constexpr s32 kRuneSettleUpdates = 30;
constexpr u32 kRuneSpellSound = 0x68;
constexpr u32 kRuneLandingSound = 0x8a;
/// The flight fades in over .8 s; e00137d0 scales the stone 1.8 times.
constexpr f32 kRuneFadeSeconds = .8f;
constexpr f32 kRuneFlightScale = 1.8f;
constexpr u64 kAllRunestones = 0x1fff;

/// Door indices in e08653bc's priority: it assigns Town, Ice, Battlefield, then Castle, and the
/// last assignment wins.
constexpr std::array<usize, 4> kDoorPriority{1, 3, 2, 0};

// e0865c98's counts and frames.
constexpr s32 kOpeningCount = 41;
constexpr s32 kDoorSpellCount = 6;
constexpr f32 kDoorChimeFrame = 30;
constexpr u32 kDoorChimeSound = 0xb9;
constexpr f32 kDoorHoldFrame = 15;
constexpr s32 kDoorHoldUpdates = 50;
constexpr f32 kDoorShimmerFrame = 16;
constexpr u32 kDoorShimmerSound = 0x6e;
constexpr f32 kDoorReleaseFrame = 19;
constexpr s32 kClosingCount = 41;

// e0863440's counts.
constexpr s32 kShardSpellCount = 51;
constexpr s32 kShardRestCount = 20;
constexpr u32 kShardChimeSound = 0x68;
/// The count at which e0863440 case 6 settles the pane and moves on.
constexpr s32 kShardSettleCount = 25;
/// Case 6's pane is opaque at count 4 and fades its white over the 20 counts after.
constexpr s32 kPaneOpaqueCount = 4;
constexpr s32 kPaneFadeCount = 20;
constexpr s32 kWindowWaitCount = 51;
constexpr s32 kWindowFadeCount = 40;
constexpr s32 kPortalFadeCount = 80;
constexpr u64 kWholeWindow = 15;

// e086098c's caption fades and the drawables' bytes.
constexpr f32 kCaptionFade = .3f;
constexpr u8 kWhite = 255;
constexpr f32 kByteScale = 255;

/** The party's progress less some of its Tower pieces. */
LegendsProgress without(LegendsProgress party, const LegendsProgress& pieces) {
    party.runestones &= ~pieces.runestones;
    party.shards &= ~pieces.shards;
    party.obelisks &= ~pieces.obelisks;
    return party;
}
} // namespace

LegendsTowerCeremony LegendsTowerCeremony::decode(std::span<const u8> overlay) {
    LegendsTowerCeremony result;
    result.m_native = formats::decodeLegendsN64Tower(overlay); // Revision authentication.
    const auto vector = [&](usize at) {
        return Vec3{std::bit_cast<f32>(readU32BE(overlay, at)),
                    std::bit_cast<f32>(readU32BE(overlay, at + 4)),
                    std::bit_cast<f32>(readU32BE(overlay, at + 8))};
    };
    result.m_runeFocus = vector(0x967c) + Vec3{0, 5, 0};
    result.m_shardFocus = vector(0x9670) + Vec3{0, 5, 0};
    constexpr std::array<usize, 4> kDoorRows{3, 1, 2, 4};
    for (usize i = 0; i < 13; ++i) {
        result.m_runeCaptions[i] = readU16BE(overlay, 0x9700 + 2 * i);
    }
    for (usize i = 0; i < 4; ++i) {
        result.m_doorFocus[i] = vector(0x9634 + kDoorRows[i] * 12) + Vec3{0, 5, 0};
        for (usize line = 0; line < 2; ++line) {
            result.m_shardCaptions[i][line] = readU16BE(overlay, 0x96ec + 4 * i + 2 * line);
            result.m_doorCaptions[i][line] =
                readU16BE(overlay, 0x96d8 + 4 * kDoorRows[i] + 2 * line);
        }
    }
    result.m_windowCaptions = {readU16BE(overlay, 0x96fc), readU16BE(overlay, 0x96fe)};
    return result;
}

void LegendsTowerCeremony::bindSequences(std::vector<Sequence> sequences) {
    m_sequences = std::move(sequences);
}

void LegendsTowerCeremony::begin(const LegendsProgress& before, const LegendsProgress& after) {
    m_started = false;
    m_entries.clear();
    m_next = 0;
    m_owned = after;
    m_withheld = {};
    m_unrevealed = {};
    m_captions.clear();
    m_sounds.clear();
    // e0862fc4 shows CATH_BEAM and the whole panes only when the seen shards make the window.
    const bool window = (before.shards & kWholeWindow) == kWholeWindow;
    m_windowReveal = window ? 1.0f : 0.0f;
    m_beamShown = window;
    m_beamTransparency = 0;
    if (!before.valid() || !after.valid()) {
        return;
    }
    m_owned.runestones |= before.runestones;
    m_owned.shards |= before.shards;
    m_owned.obelisks |= before.obelisks;
    // e0862220 selects the lowest new stone, captioned by e086aec8's count of the seen ones, and
    // sets e086af00 when the stones make all thirteen.
    if (const u64 runes = after.runestones & ~before.runestones & kAllRunestones; runes != 0) {
        const auto seen = static_cast<usize>(std::popcount(before.runestones & kAllRunestones));
        const bool whole = (m_owned.runestones & kAllRunestones) == kAllRunestones;
        m_entries.push_back({.kind = Kind::Rune,
                             .index = static_cast<usize>(std::countr_zero(runes)),
                             .captions = {m_runeCaptions[seen]},
                             .completesRunes = whole});
        m_withheld.runestones = runes;
        m_unrevealed.runestones = whole ? runes : 0;
    }
    // e08653bc selects the newly opened door it ranks first; e0864e7c builds the others open.
    for (const usize i : kDoorPriority) {
        const auto required = LegendsTowerProgress::kDoorObelisks[i];
        if ((after.obelisks & required) == required && (before.obelisks & required) != required) {
            m_entries.push_back({Kind::Door, i, captions(m_doorCaptions[i])});
            m_withheld.obelisks = required & ~before.obelisks;
            break;
        }
    }
    // e0863364 selects the highest new shard and sets e086ae04 when the shards make the window.
    if (const u64 shards = after.shards & ~before.shards & kWholeWindow; shards != 0) {
        const auto index = static_cast<usize>(std::bit_width(shards)) - 1;
        const bool whole = (m_owned.shards & kWholeWindow) == kWholeWindow;
        m_entries.push_back({.kind = Kind::Shard,
                             .index = index,
                             .captions = captions(m_shardCaptions[index]),
                             .completesWindow = whole});
        m_withheld.shards = shards;
        m_unrevealed.shards = whole ? shards : 0;
    }
}

LegendsProgress LegendsTowerCeremony::displayed(const LegendsProgress& party) const {
    return without(party, m_withheld);
}

LegendsProgress LegendsTowerCeremony::completed(const LegendsProgress& party) const {
    return without(party, m_unrevealed);
}

const LegendsTowerCeremony::Entry* LegendsTowerCeremony::current() const {
    return pending() ? &m_entries[m_next] : nullptr;
}

void LegendsTowerCeremony::start(const WorldCamera& camera, const Vec3& attention) {
    if (!pending() || active()) {
        return;
    }
    m_started = true;
    m_remainder = 0;
    m_dispatch = Dispatch::Select;
    m_returnCamera = camera;
    m_returnFocus = attention;
    // e0860620 cuts every camera channel to Sumner's shot: e007d1d4, e007d53c and
    // e007d584 each set the channel's current value to its target.
    const Vec3 focus = m_native.sumner.position + Vec3{0, kSumnerFocusHeight, 0};
    m_camera = shotCamera(camera, focus, kShotDistance, kSumnerShotPitch);
    m_previousCamera = m_cameraFrom = m_cameraGoal = m_camera;
    m_cameraFocus = m_focusFrom = m_focusGoal = focus;
    m_glide = {};
    // It also empties the caption queue and lifts the ready suppression.
    m_captions.clear();
    m_readySuppressed = false;
    m_flightFrame = -1;
    m_paneBytes.reset();
    m_gesture = {};
    play(kReady);
    passGesture();
}

f32 LegendsTowerCeremony::partyFacing(const Vec3& position) const {
    // e0860620 passes e0010e50 Sumner's +0x1c and +0x24 less the player's +0x24 and +0x2c.
    const Vec3 offset = m_native.sumner.position - position;
    return portable::atan2(offset.x, offset.z);
}

void LegendsTowerCeremony::play(std::string_view name) {
    // e002f708 leaves the current sequence ended and frozen (0x9000) when it finds none.
    const auto* found = findSequence(m_sequences, name);
    if (!found) {
        m_gesture.done = true;
        m_gesture.frozen = true;
        return;
    }
    // e002f7a0: frame 0, held through the next pass, at 16 / rate frames a pass. Its
    // mask 0xfffb6f00 keeps the Tower's once flag and the previous frame (+0x30).
    m_gesture.name = found->name;
    m_gesture.frames = found->frames;
    m_gesture.step = kTick / std::max(found->secondsPerFrame, kMinimumPeriod);
    m_gesture.repeats = found->repeats;
    m_gesture.frame = 0;
    m_gesture.reversed = false;
    m_gesture.held = true;
    m_gesture.done = false;
    m_gesture.frozen = false;
}

void LegendsTowerCeremony::perform(std::string_view name) {
    // e0860ca4 plays every gesture but REDY once; REDY keeps the flag it inherits, so
    // the REDY after MKRU plays once too.
    play(name);
    if (name != kReady) {
        m_gesture.once = true;
    }
}

void LegendsTowerCeremony::passGesture() {
    // e00327e4 as e0032c80 runs it after the callback, skipping a frozen object.
    auto& gesture = m_gesture;
    if (gesture.frozen || gesture.frames <= 0) {
        return;
    }
    const auto count = static_cast<f32>(gesture.frames);
    f32 frame = gesture.held ? gesture.frame : gesture.frame + gesture.step;
    bool ended = count <= frame;
    gesture.previousFrame = gesture.frame;
    if (ended) {
        frame -= count;
    }
    gesture.frame = frame;
    // A loop wraps and raises only the end flag; a once-sequence clamps at its last
    // frame and freezes on the pass that reaches it.
    const bool once = gesture.once || !gesture.repeats;
    if (once && (count - 1 <= frame || ended)) {
        gesture.frame = count - 1;
        ended = true;
    }
    gesture.held = false;
    if (ended) {
        gesture.done = true;
        gesture.frozen = once;
    }
}

bool LegendsTowerCeremony::crossed(f32 frame) const {
    return m_gesture.previousFrame < frame && frame <= m_gesture.frame;
}

f32 LegendsTowerCeremony::gestureFrame() const {
    if (!m_gesture.reversed) {
        return m_gesture.frame;
    }
    // e00327e4 shows a reversed once-sequence from its last frame, a loop from its count.
    if (m_gesture.frames < 2) {
        return 0;
    }
    const bool once = m_gesture.once || !m_gesture.repeats;
    const f32 shown = static_cast<f32>(m_gesture.frames - (once ? 1 : 0)) - m_gesture.frame;
    return shown < 0 ? shown + static_cast<f32>(m_gesture.frames) : shown;
}

void LegendsTowerCeremony::queueCaptions(std::span<const u32> ids) {
    for (const u32 id : ids) {
        if (captionHasText(id)) {
            m_captions.push_back({.id = id});
        }
    }
}

void LegendsTowerCeremony::stepCaptions() {
    // e086098c runs the front caption's float32 clock: it fades in over its first .3 s,
    // fades out over its last and goes when its time reaches 0, on its 91st update.
    if (m_captions.empty()) {
        return;
    }
    auto& front = m_captions.front();
    front.remaining = std::max(front.remaining - kTick, 0.0f);
    const auto faded = [](f32 seconds) {
        return static_cast<u8>(static_cast<s32>((1 - seconds / kCaptionFade) * kByteScale));
    };
    switch (front.stage) {
    case Caption::Stage::Queued: front.stage = Caption::Stage::FadingIn; [[fallthrough]];
    case Caption::Stage::FadingIn: {
        const f32 elapsed = kCaptionSeconds - front.remaining;
        if (elapsed >= kCaptionFade) {
            front.transparency = 0;
            front.stage = Caption::Stage::Shown;
        } else {
            front.transparency = faded(elapsed);
        }
        break;
    }
    case Caption::Stage::Shown:
        // The update that starts the fade-out keeps the caption's transparency.
        if (front.remaining < kCaptionFade) {
            front.stage = Caption::Stage::FadingOut;
        }
        break;
    case Caption::Stage::FadingOut: front.transparency = faded(front.remaining); break;
    }
    if (front.remaining <= 0) {
        m_captions.erase(m_captions.begin());
    }
}

void LegendsTowerCeremony::attend() {
    // e0860d1c restarts REDY, looping, once a gesture that plays once has ended (0x9000),
    // unless the shard spell suppresses it. Sumner's turn toward the shot is not modelled.
    if (!m_readySuppressed && m_gesture.done && m_gesture.frozen) {
        play(kReady);
        m_gesture.once = false;
    }
    stepCaptions();
}

void LegendsTowerCeremony::select() {
    // Case 1 runs the next pending entry's setup, or picks the return (0x28).
    const auto* entry = current();
    if (!entry) {
        m_dispatch = Dispatch::PlanReturn;
        return;
    }
    m_dispatch = Dispatch::Script;
    m_count = 0;
    m_paneBytes.reset();
    switch (entry->kind) {
    case Kind::Rune:
        // e08622f4 records the flight (e08618a8), then runs e086239c's time-0 events
        // with no time step: the caption is queued here and shown on the next update.
        makeRuneFlight(entry->index);
        m_runeTime = 0;
        m_runeStage = RuneStage::Waiting;
        m_spellAt = -1;
        m_flightStart = -1;
        m_settledAt = -1;
        m_flightFrame = -1;
        runeScript(*entry);
        break;
    case Kind::Door:
        // e086544c, then one e0865c98 call: a camera step, the captions, the first count.
        m_doorCase = DoorCase::Captions;
        m_doorHold = -1;
        m_doorBolt = false;
        advanceCamera();
        doorScript(*entry);
        break;
    case Kind::Shard:
        // e0863410 only resets the script.
        m_shardCase = ShardCase::Captions;
        break;
    }
}

bool LegendsTowerCeremony::runScript(const Entry& entry) {
    // Each script steps the camera (e007dd70) before its own case.
    advanceCamera();
    switch (entry.kind) {
    case Kind::Rune: ++m_runeTime; return runeScript(entry);
    case Kind::Door: return doorScript(entry);
    case Kind::Shard: return shardScript(entry);
    }
    return false;
}

bool LegendsTowerCeremony::runeScript(const Entry& entry) {
    // e086239c's events fall on whole updates since its setup. An event slot marked
    // on one update is stamped and acts on the next (-0x29a, 0x1046a, then 1).
    const s32 now = m_runeTime;
    if (now == 0) {
        queueCaptions(entry.captions);
    }
    if (now == kRuneShotTime) {
        shot(m_runeFocus, kShotDistance, kRuneShotPitch);
    }
    if (now == kRuneSpellTime) {
        m_sounds.push_back(kRuneSpellSound);
        m_spellAt = now + 1;
    }
    if (m_flightStart >= 0 && now >= m_flightStart) {
        const s32 pose = now - m_flightStart;
        if (pose < static_cast<s32>(kFlightPoses)) {
            m_flightFrame = pose;
        } else if (pose == static_cast<s32>(kFlightPoses)) {
            // The stone lands in its final pose with 0x8a; the settle slot follows.
            m_flightFrame = -1;
            m_withheld.runestones &= ~(u64{1} << entry.index);
            m_sounds.push_back(kRuneLandingSound);
            m_settledAt = now + 1;
        }
    }
    // The thirteenth stone returns at its settle stamp (slot 0), the others 30 later. That
    // stamp also turns the rune portal on (e086aec0).
    if (m_settledAt >= 0 && now == m_settledAt + (entry.completesRunes ? 0 : kRuneSettleUpdates)) {
        m_unrevealed.runestones = 0;
        return true;
    }
    // The stage is read before MKRU starts, so the release test first runs on the next update.
    const RuneStage stage = m_runeStage;
    if (now == m_spellAt) {
        perform(kRuneSpell);
        m_gesture.step *= kRuneSpellSpeed;
        m_runeStage = RuneStage::Spell;
    }
    if (stage == RuneStage::Spell) {
        // The release test reads frame 14 of whatever Sumner plays: MKRU, or the REDY that
        // e0860d1c starts when MKRU is absent. A gesture that ends short of it would leave
        // retail waiting forever; the stone goes as it ends instead.
        if (m_flightStart < 0 && (crossed(kRuneReleaseFrame) || m_gesture.done)) {
            m_flightStart = now + 1;
        }
        if (m_gesture.done) {
            perform(kReady);
            m_runeStage = RuneStage::Ready;
        }
    }
    return false;
}

bool LegendsTowerCeremony::doorScript(const Entry& entry) {
    switch (m_doorCase) {
    case DoorCase::Captions:
        queueCaptions(entry.captions);
        m_count = 0;
        m_doorCase = DoorCase::Opening;
        [[fallthrough]];
    case DoorCase::Opening:
        if (++m_count >= kOpeningCount) {
            m_doorCase = DoorCase::Shot;
        }
        break;
    case DoorCase::Shot:
        shot(m_doorFocus[entry.index], kShotDistance, kDoorShotPitch);
        m_count = 0;
        m_doorCase = DoorCase::Spell;
        break;
    case DoorCase::Spell:
        if (++m_count >= kDoorSpellCount) {
            perform(kDoorSpell);
            m_gesture.reversed = true;
            m_doorCase = DoorCase::SpellReversed;
        }
        break;
    case DoorCase::SpellReversed:
        // The chime comes as the reversed STRT's own count passes 30; its end starts STRT forward.
        if (crossed(kDoorChimeFrame)) {
            m_sounds.push_back(kDoorChimeSound);
        }
        if (m_gesture.done) {
            perform(kDoorSpell);
            m_doorCase = DoorCase::SpellForward;
        }
        break;
    case DoorCase::SpellForward:
        if (m_gesture.done) {
            perform(kDoorRelease);
            m_count = 0;
            m_doorHold = -1;
            m_doorCase = DoorCase::Bolt;
        }
        break;
    case DoorCase::Bolt: {
        // ODOR freezes at frame 15 for a countdown taken from the update that sees it; the
        // bolt it sends chimes once past frame 16, and frame 19 lets the door go.
        if (!m_gesture.frozen && crossed(kDoorHoldFrame)) {
            m_doorHold = kDoorHoldUpdates;
            m_gesture.frozen = true;
            m_doorBolt = true;
        }
        if (m_doorHold >= 0) {
            --m_doorHold;
            if (m_doorHold < 0) {
                m_gesture.frozen = false;
            }
        }
        if (m_doorBolt && m_gesture.frame > kDoorShimmerFrame) {
            m_doorBolt = false;
            m_sounds.push_back(kDoorShimmerSound);
        }
        // Case 6 lets the door go past frame 19 of whatever Sumner plays: when ODOR ends
        // short of it or is absent, the REDY e0860d1c starts holds at 15 and passes 19.
        // The door also goes as any gesture ends with no REDY bound, and as REDY itself
        // ends, where retail would wait forever; for REDY that holds while it adds at most
        // four frames a pass, as every shipped rate does. A faster REDY can jump from under
        // 15 past 19 on a later lap, and retail lets the door go then, with a second 0x6e.
        const bool stalled = m_gesture.done && (m_gesture.name == kReady ||
                                                findSequence(m_sequences, kReady) == nullptr);
        if (m_gesture.frame > kDoorReleaseFrame || stalled) {
            m_count = 0;
            m_doorCase = DoorCase::Release;
        }
        break;
    }
    case DoorCase::Release:
        // The door's held animation goes on; the adapter plays 0x10 as the door shows open.
        m_count = 0;
        m_withheld.obelisks &= ~u64{LegendsTowerProgress::kDoorObelisks[entry.index]};
        m_doorCase = DoorCase::Closing;
        break;
    case DoorCase::Closing:
        if (++m_count >= kClosingCount) {
            m_doorCase = DoorCase::Finished;
        }
        break;
    case DoorCase::Finished: return true;
    }
    return false;
}

bool LegendsTowerCeremony::shardScript(const Entry& entry) {
    switch (m_shardCase) {
    case ShardCase::Captions:
        queueCaptions(entry.captions);
        m_count = 0;
        m_shardCase = ShardCase::Opening;
        [[fallthrough]];
    case ShardCase::Opening:
        if (++m_count >= kOpeningCount) {
            m_shardCase = ShardCase::Shot;
        }
        break;
    case ShardCase::Shot:
        shot(m_shardFocus, kShotDistance, kShardShotPitch);
        m_count = 0;
        m_shardCase = ShardCase::Spell;
        break;
    case ShardCase::Spell:
        if (++m_count >= kShardSpellCount) {
            perform(kShardSpell);
            m_readySuppressed = true;
            m_shardCase = ShardCase::Install;
        }
        break;
    case ShardCase::Install:
        // The pane shows, clear and white, once SHD1 has ended.
        if (m_gesture.done) {
            m_withheld.shards &= ~(u64{1} << entry.index);
            m_paneBytes = PaneBytes{kTransparent, kWhite};
            m_count = 0;
            m_shardCase = ShardCase::Rest;
        }
        break;
    case ShardCase::Rest:
        // Counts 0 to 20 keep the clear pane; the 22nd update plays 0x68.
        if (m_count > kShardRestCount) {
            m_count = 0;
            m_shardCase = ShardCase::Settle;
            m_sounds.push_back(kShardChimeSound);
        }
        ++m_count;
        break;
    case ShardCase::Settle:
        if (m_count < kShardSettleCount) {
            m_paneBytes = paneBytes(m_count);
        } else {
            // Settled at transparency 208 and no white, as every installed pane shows.
            m_paneBytes.reset();
            m_shardCase = ShardCase::Restore;
        }
        ++m_count;
        break;
    case ShardCase::Restore:
        perform(kShardRestore);
        m_readySuppressed = false;
        m_count = 0;
        if (!entry.completesWindow) {
            m_shardCase = ShardCase::Closing;
            break;
        }
        // A shard that makes the window (e086ae04) turns to it, queues its captions and
        // unhides CATH_BEAM fully transparent; the completed window shows from here, even
        // with a new pane left for the next visit.
        shot(m_shardFocus - Vec3{0, kWindowFocusDrop, 0}, kWindowShotDistance, kWindowShotPitch);
        queueCaptions(captions(m_windowCaptions));
        m_beamShown = true;
        m_beamTransparency = kTransparent;
        m_unrevealed.shards = 0;
        m_shardCase = ShardCase::WindowWait;
        break;
    case ShardCase::WindowWait:
        // Hands over on its 52nd update.
        if (m_count < kWindowWaitCount) {
            ++m_count;
        } else {
            m_count = 0;
            m_shardCase = ShardCase::Reveal;
        }
        break;
    case ShardCase::Reveal: {
        // Counts 0 to 40 fade the beam and the panes in; the portal's parts fade on to 80.
        const s32 fading = std::min(m_count, kWindowFadeCount);
        m_windowReveal = static_cast<f32>(fading) / kWindowFadeCount;
        m_beamTransparency =
            static_cast<u8>((kWindowFadeCount - fading) * kTransparent / kWindowFadeCount);
        if (m_count < kPortalFadeCount) {
            ++m_count;
        } else {
            m_count = 0;
            m_shardCase = ShardCase::Revealed;
        }
        break;
    }
    case ShardCase::Revealed:
        m_count = 0;
        m_shardCase = ShardCase::Closing;
        break;
    case ShardCase::Closing:
        if (++m_count >= kClosingCount) {
            m_shardCase = ShardCase::Finished;
        }
        break;
    case ShardCase::Finished: return true;
    }
    return false;
}

void LegendsTowerCeremony::tick() {
    m_previousCamera = m_camera;
    bool attends = false;
    switch (m_dispatch) {
    case Dispatch::Select: select(); break;
    case Dispatch::Script:
        if (runScript(m_entries[m_next])) {
            // A finished script's pieces stop pending; case 1 selects on the next update.
            ++m_next;
            m_dispatch = Dispatch::Select;
        }
        attends = true;
        break;
    case Dispatch::PlanReturn:
        // Case 0x28 plans the return to the gameplay camera; it first moves next update.
        glideTo(m_returnCamera, m_returnFocus, kReturnAcceleration);
        m_dispatch = Dispatch::Return;
        attends = true;
        break;
    case Dispatch::Return:
        if (advanceCamera()) {
            m_dispatch = Dispatch::HandBack;
        }
        attends = true;
        break;
    case Dispatch::HandBack:
        // Case 0x3c (e0861260) cuts the captions, restarts REDY and resumes play. Ownership
        // was committed before the ceremony began; the pieces it left undrawn stay so for
        // the rest of the visit.
        m_captions.clear();
        play(kReady);
        m_started = false;
        break;
    }
    if (attends) {
        attend();
    }
    passGesture();
}

void LegendsTowerCeremony::update(f32 seconds) {
    if (!active() || !std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    m_remainder += std::min(seconds, 1.0f);
    while (m_remainder + 1e-9 >= kTick && active()) {
        m_remainder -= kTick;
        tick();
    }
}

void LegendsTowerCeremony::shot(const Vec3& focus, f32 distance, f32 pitch) {
    glideTo(shotCamera(m_camera, focus, distance, pitch), focus, kShotAcceleration);
}

void LegendsTowerCeremony::glideTo(const WorldCamera& goal, const Vec3& focus, f32 acceleration) {
    m_cameraFrom = m_camera;
    m_focusFrom = m_cameraFocus;
    m_cameraGoal = goal;
    m_focusGoal = focus;
    if (goal == m_camera && focus == m_cameraFocus) {
        m_glide = {}; // e007cdf0 finds nothing to move.
    } else {
        m_glide.plan(acceleration);
    }
}

bool LegendsTowerCeremony::advanceCamera() {
    if (!m_glide.advance(kTick)) {
        m_camera = m_cameraGoal.interpolate(m_cameraFrom, m_glide.travelled);
        m_cameraFocus = glm::mix(m_focusFrom, m_focusGoal, m_glide.travelled);
        return false;
    }
    m_camera = m_cameraGoal;
    m_cameraFocus = m_focusGoal;
    return true;
}

void LegendsTowerCeremony::Glide::plan(f32 accelerationSeconds) {
    // e007cdf0 for a move of length one; every channel of a shot shares these times.
    acceleration = accelerationSeconds;
    elapsed = 0;
    speed = 0;
    travelled = 0;
    cruiseSpeed = 1 / ((acceleration + kBrakingSeconds) * .5f + kCruiseSeconds);
    fall = -cruiseSpeed / kBrakingSeconds;
    rise = acceleration > 0 ? cruiseSpeed / acceleration : 0;
    phase = acceleration > 0 ? Phase::Accelerating : Phase::Cruising;
}

bool LegendsTowerCeremony::Glide::advance(f32 seconds) {
    // e007d774 in float32: a step that overruns a phase spends the rest in the next, and
    // the move ends on the step that overruns the braking.
    if (phase == Phase::Done) {
        return true;
    }
    f32 rest = seconds;
    f32 moved = 0;
    if (phase == Phase::Accelerating) {
        f32 slice = rest;
        const f32 left = acceleration - elapsed;
        if (rest <= left) {
            elapsed += rest;
            rest = 0;
        } else {
            slice = left;
            rest -= left;
            elapsed = 0;
            phase = Phase::Cruising;
        }
        const f32 before = speed;
        speed = before + rise * slice;
        moved = (before + rise * slice * .5f) * slice;
    }
    if (phase == Phase::Cruising) {
        f32 slice = rest;
        const f32 left = kCruiseSeconds - elapsed;
        speed = cruiseSpeed;
        if (rest <= left) {
            elapsed += rest;
            rest = 0;
        } else {
            slice = left;
            rest -= left;
            elapsed = 0;
            phase = Phase::Braking;
        }
        moved += speed * slice;
    }
    if (phase == Phase::Braking) {
        f32 slice = rest;
        const f32 left = kBrakingSeconds - elapsed;
        if (rest <= left) {
            elapsed += rest;
        } else {
            slice = left;
            elapsed = 0;
            phase = Phase::Done;
        }
        const f32 before = speed;
        speed = before + fall * slice;
        moved += (before + fall * slice * .5f) * slice;
    }
    if (phase == Phase::Done) {
        speed = 0;
        travelled = 1;
        return true;
    }
    if (moved > 0) {
        travelled += moved;
    }
    return false;
}

WorldCamera LegendsTowerCeremony::camera(f32 alpha) const {
    return alpha < 0 ? m_camera
                     : m_camera.interpolate(m_previousCamera, std::clamp(alpha, 0.0f, 1.0f));
}

void LegendsTowerCeremony::makeRuneFlight(usize index) {
    // e08618a8 records 62 poses starting at the final floor placement, then
    // e086239c reads the recording backwards. This is not a hand-to-floor lerp.
    const auto& final = m_native.runestones[index].placement;
    FlightPose pose{Vec3{final[3]}, {kHalfPi, 0, 0}};
    const Vec3 centre = m_runeFocus - Vec3{0, 5, 0};
    pose.angles.y = std::atan2(centre.x - pose.position.x, centre.z - pose.position.z);
    std::minstd_rand random{static_cast<u32>(index + 1)};
    Vec3 spin;
    for (s32 axis = 0; axis < 3; ++axis) {
        do {
            spin[axis] = .08f - static_cast<f32>(random() % 32768) * .16f / 32768;
        } while (spin[axis] < .012f);
    }
    Vec3 velocity = -2.0f * spin;
    const Vec3 acceleration = -.25f * velocity;
    f32 ySpeed = .1f;
    m_flight[0] = pose;
    usize at = 1;
    for (s32 i = 0; i < 8; ++i) {
        velocity += acceleration;
        pose.angles += velocity;
        ySpeed -= .025f;
        pose.position.y += ySpeed;
        m_flight[at++] = pose;
    }
    for (s32 i = 0; i < 3; ++i) {
        pose.position.y += .1f;
        pose.angles += spin;
        m_flight[at++] = pose;
    }
    velocity = Vec3{0};
    ySpeed = .7f;
    for (s32 i = 0; i < 50; ++i) {
        velocity = glm::clamp(velocity + spin, Vec3{-kPi / 6}, Vec3{kPi / 6});
        ySpeed = std::max(0.0f, ySpeed - .04f);
        pose.position.y += ySpeed;
        pose.angles += velocity;
        m_flight[at++] = pose;
    }
}

Mat4 LegendsTowerCeremony::runePlacement() const {
    if (!current() || current()->kind != Kind::Rune || m_flightFrame < 0) {
        return Mat4{1};
    }
    // Flight pose k shows recorded pose 61 - k.
    constexpr auto kLast = static_cast<s32>(kFlightPoses) - 1;
    const auto& pose = m_flight[static_cast<usize>(kLast - std::min(m_flightFrame, kLast))];
    return glm::translate(Mat4{1}, pose.position) *
           glm::rotate(Mat4{1}, pose.angles.y, Vec3{0, 1, 0}) *
           glm::rotate(Mat4{1}, -pose.angles.x, Vec3{1, 0, 0}) *
           glm::rotate(Mat4{1}, -pose.angles.z, Vec3{0, 0, 1}) *
           glm::scale(Mat4{1}, Vec3{kRuneFlightScale});
}

f32 LegendsTowerCeremony::runeOpacity() const {
    return std::clamp(static_cast<f32>(m_flightFrame) * kTick / kRuneFadeSeconds, 0.0f, 1.0f);
}

LegendsTowerCeremony::PaneBytes LegendsTowerCeremony::paneBytes(s32 count) {
    // e0863440 case 6 stores +1, the transparency (count - 4) * 208 / 20, through sb, so
    // counts 1 to 3 wrap (Tower e08638a0..e08638d0), and +5, the white blend
    // ~((count - 4)^2 * 255 / 400). game e001ea34 draws 255 less the transparency, so
    // counts 1 to 3 flash 30, 19 and 9 before the opaque pop at 4.
    const s32 offset = count - kPaneOpaqueCount;
    return {static_cast<u8>(offset * LegendsTowerProgress::kShardTransparency / kPaneFadeCount),
            static_cast<u8>(kWhite - offset * offset * kWhite / (kPaneFadeCount * kPaneFadeCount))};
}

f32 LegendsTowerCeremony::shardOpacity() const {
    // e001ea34 draws 255 less the drawable's transparency byte.
    if (m_paneBytes) {
        return static_cast<f32>(kTransparent - m_paneBytes->transparency) / kByteScale;
    }
    return (kByteScale - LegendsTowerProgress::kShardTransparency * (1 - m_windowReveal)) /
           kByteScale;
}

f32 LegendsTowerCeremony::shardWhiteBlend() const {
    return m_paneBytes ? static_cast<f32>(m_paneBytes->white) / kByteScale : 0;
}

f32 LegendsTowerCeremony::cathedralBeamOpacity() const {
    return m_beamShown ? static_cast<f32>(kTransparent - m_beamTransparency) / kByteScale : 0;
}

std::optional<u32> LegendsTowerCeremony::caption() const {
    if (m_captions.empty() || m_captions.front().stage == Caption::Stage::Queued) {
        return std::nullopt;
    }
    return m_captions.front().id;
}

f32 LegendsTowerCeremony::captionOpacity() const {
    return caption() ? static_cast<f32>(kTransparent - m_captions.front().transparency) / kByteScale
                     : 0;
}

std::vector<u32> LegendsTowerCeremony::takeSounds() {
    return std::exchange(m_sounds, {});
}

} // namespace gdl::game
