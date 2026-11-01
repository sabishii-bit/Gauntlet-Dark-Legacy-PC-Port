#pragma once

#include <compare>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "engine/audio/SoundPlayer.h"

#include "game/rollback/FrameEventLedger.h"

namespace gdl::game {

/** A simulation cue, not a mixer voice. Safe to retain in native scene history. */
struct SoundCueToken {
    FrameEventIdentity event;
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const SoundCueToken& state) {
        visitor.template fields<1>(state);
    }
    explicit operator bool() const { return event.valid() && event.tick != 0; }
    auto operator<=>(const SoundCueToken&) const = default;
};

struct SceneSoundResource {
    std::string bank;
    u32 call = 0;
    auto operator<=>(const SceneSoundResource&) const = default;
};

enum class SceneSoundPlayback : u8 { RepeatWhileDesired, PlayOnceUntilCancelled };

/** Immutable resource identity and value parameters; no mixer handles or sample pointers. */
struct SceneSoundPayload {
    SceneSoundResource resource;
    f32 volume = 1;
    f32 pan = 0;
    std::optional<SoundCueToken> after;
    SceneSoundPlayback playback = SceneSoundPlayback::PlayOnceUntilCancelled;
    SoundCueToken start;
    bool operator==(const SceneSoundPayload&) const = default;
};

/** Gameplay audio boundary. Capture a complete tick, replace the corrected
 * journal suffix, then dispatch the ledger's delta after replay. Resource clips
 * remain borrowed from the same loaded stage; close before releasing its banks.
 * Owned finite cues and ambient loops use complete desired lifetimes. Music,
 * pause controls and irreversible effects have separate presentation lifecycles. */
class SceneSoundEvents {
public:
    using Ledger = FrameEventLedger<SceneSoundPayload>;
    using Journal = Ledger::Journal;
    using Result = Ledger::Result;
    struct DesiredLoop {
        FrameLoopKey key;
        SceneSoundPayload payload;
        bool operator==(const DesiredLoop&) const = default;
    };
    struct Frame {
        Journal finite;
        std::vector<DesiredLoop> loops; ///< complete final desire, not just rising edges
        bool operator==(const Frame&) const = default;
    };
    class Unsupported final : public std::logic_error {
    public:
        explicit Unsupported(const std::string& reason) : std::logic_error(reason) {}
    };

    class Scope {
    public:
        Scope(SceneSoundEvents& owner, FrameEventEmitter emitter, u32 cue);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

    private:
        friend class SceneSoundEvents;
        SceneSoundEvents& m_owner;
        std::optional<std::pair<FrameEventEmitter, u32>> m_previous;
        u64 m_capture = 0;
        int m_exceptions = 0;
    };

    SceneSoundEvents() = default;
    SceneSoundEvents(const SceneSoundEvents&) = delete;
    SceneSoundEvents& operator=(const SceneSoundEvents&) = delete;
    ~SceneSoundEvents() = default;
    void open(u64 epoch, SoundPlayer* output, u64 firstTick = 1);
    void close();
    /** Closes and forgets the epoch's history, so that a scene returned to the start of that
     * epoch can record the same epoch again from its first tick. */
    void discard();
    void beginTick(u64 tick);
    Journal endTick();
    Frame endFrame();
    /** Discard only the unfinished journal. Scope unwinding also aborts when an
     * exception escapes. Validation errors caught by a caller leave its earlier
     * accepted events intact; it may explicitly abort instead of publishing.
     * Resource cache entries are immutable and may survive an aborted capture. */
    void abortTick() noexcept;
    bool recording() const { return m_recording; }
    u64 epoch() const { return m_epoch; }
    Scope scoped(FrameEventEmitter emitter, u32 cue) { return {*this, emitter, cue}; }
    SoundCueToken emit(const SoundSequence& sequence, f32 volume = 1, f32 pan = 0,
                       SoundCueToken after = {});
    /** Native start of a cancellable finite cue. No sound is emitted until its
     * owner includes this token in the final desired-loop set. */
    SoundCueToken beginCue(const SoundSequence& sequence);
    void desireLoop(const FrameLoopKey& key, const SoundSequence& sequence,
                    SceneSoundPlayback playback, f32 volume = 1, f32 pan = 0,
                    SoundCueToken start = {});
    /** No output mutation; only the completed replay's ledger delta is presented. */
    Result reconcile(u64 firstTick, std::span<const Journal> journals);
    Result reconcileFrames(u64 firstTick, std::span<const Frame> frames,
                           std::optional<u64> newHead = std::nullopt);
    Result confirm(u64 allSeatsThrough);
    /** Repeated delivery within retained history is harmless. A delta older than
     * the history window is rejected, even after its original voice has ended. */
    void dispatch(const Result& result);
    /** Local presentation service, once after replay, never once per replayed tick. */
    void servicePlayback();
    void pausePlayback();
    void resumePlayback();
    /** Fails closed so integration tests cannot mistake unrecorded output for coverage. */
    [[noreturn]] void unsupported(std::string_view operation);
    u64 unsupportedCount() const { return m_unsupported; }
    u64 recordedCount() const { return m_recorded; }
    const std::string& lastUnsupported() const { return m_lastUnsupported; }

private:
    SceneSoundResource bindResource(const SoundSequence& sequence);
    SoundCueToken nextCue();
    void startLoop(const FrameLoopKey& key, const SceneSoundPayload& payload);
    Ledger m_ledger;
    u64 m_epoch = 0;
    u64 m_firstTick = 0;
    u64 m_dispatchFloor = 0;
    u64 m_tick = 0;
    u64 m_capture = 0;
    bool m_recording = false;
    Journal m_journal;
    std::vector<DesiredLoop> m_desired;
    std::map<u64, std::vector<DesiredLoop>> m_loopFrames;
    std::vector<DesiredLoop> m_loopBase;
    std::optional<std::pair<FrameEventEmitter, u32>> m_scope;
    std::map<std::pair<FrameEventEmitter, u32>, u32> m_occurrences;
    struct ResourceBinding {
        SoundSequence sequence;
        // Another owner may decode the same native call independently. Check
        // its immutable samples once, then reuse the verified pointer binding.
        std::vector<SoundSequence> equivalents;
    };
    std::map<SceneSoundResource, ResourceBinding> m_resources;
    std::set<FrameEventIdentity> m_dispatched;
    std::map<FrameEventIdentity, SoundHandle> m_handles;
    struct PlayingLoop {
        SceneSoundPayload payload;
        SoundHandle handle = kNoSound;
        bool finished = false;
    };
    std::map<FrameLoopKey, PlayingLoop> m_loops;
    bool m_paused = false;
    SoundPlayer* m_output = nullptr;
    u64 m_unsupported = 0;
    u64 m_recorded = 0;
    std::string m_lastUnsupported;
};

} // namespace gdl::game
