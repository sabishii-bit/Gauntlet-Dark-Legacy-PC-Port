#include "game/audio/SceneSoundEvents.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

#include "engine/core/Log.h"

namespace gdl::game {
namespace {
/** Cue ids and loop channels stay under the tag bits that mark a loop's start and stop
 * events by their channel. */
constexpr u32 kMostCues = 0x80000000U;
constexpr u32 kMostChannels = 0x40000000U;
constexpr u32 kLoopStartTag = 0x80000000U;
constexpr u32 kLoopStopTag = 0xc0000000U;

bool sameBinding(const SoundSequence& left, const SoundSequence& right) {
    return left.volume == right.volume && left.gainCurve == right.gainCurve &&
           std::ranges::equal(left.steps, right.steps, [](const auto& a, const auto& b) {
               return a.clip == b.clip && a.loopStart == b.loopStart && a.loopBack == b.loopBack;
           });
}

bool sameContent(const SoundSequence& left, const SoundSequence& right) {
    return left.volume == right.volume && left.gainCurve == right.gainCurve &&
           std::ranges::equal(left.steps, right.steps, [](const auto& a, const auto& b) {
               if (a.loopStart != b.loopStart || a.loopBack != b.loopBack) {
                   return false;
               }
               return a.clip == b.clip ||
                      (a.clip != nullptr && b.clip != nullptr &&
                       a.clip->sampleRate == b.clip->sampleRate &&
                       a.clip->channels == b.clip->channels && a.clip->samples == b.clip->samples);
           });
}

bool validPredecessor(const SoundCueToken& after, u64 epoch, u64 tick) {
    return after && after.event.epoch == epoch && after.event.tick <= tick;
}
} // namespace

SceneSoundEvents::Scope::Scope(SceneSoundEvents& owner, FrameEventEmitter emitter, u32 cue)
    : m_owner(owner), m_previous(owner.m_scope), m_capture(owner.m_capture),
      m_exceptions(std::uncaught_exceptions()) {
    if (!owner.m_recording || !emitter.valid() || cue == 0 || cue >= kMostCues) {
        // A failed constructor has no destructor to unwind the capture.
        owner.abortTick();
        owner.unsupported("sound emitter scope outside a native tick or without a stable identity");
    }
    owner.m_scope = std::pair{emitter, cue};
}
SceneSoundEvents::Scope::~Scope() {
    if (!m_owner.m_recording || m_owner.m_capture != m_capture) {
        return;
    }
    if (std::uncaught_exceptions() > m_exceptions) {
        m_owner.abortTick();
    } else {
        m_owner.m_scope = m_previous;
    }
}

[[noreturn]] void SceneSoundEvents::unsupported(std::string_view operation) {
    ++m_unsupported;
    m_lastUnsupported = operation;
    throw Unsupported(m_lastUnsupported);
}

void SceneSoundEvents::open(u64 epoch, SoundPlayer* output, u64 firstTick) {
    if (m_recording) {
        unsupported("rebinding sound resources during a native tick");
    }
    if (firstTick == 0) {
        unsupported("sound history starts at a nonzero completed native tick");
    }
    const auto result = m_ledger.reset(epoch, firstTick);
    if (!result) {
        unsupported("sound epoch must be nonzero and different from the previous binding");
    }
    close();
    m_epoch = epoch;
    m_firstTick = m_dispatchFloor = firstTick;
    m_output = output;
    m_unsupported = m_recorded = 0;
    m_lastUnsupported.clear();
}

void SceneSoundEvents::close() {
    if (m_recording) {
        unsupported("closing sound resources during a native tick");
    }
    if (m_output) {
        for (const auto& [identity, handle] : m_handles) {
            m_output->stop(handle);
        }
        for (const auto& [key, loop] : m_loops) {
            m_output->stop(loop.handle);
        }
    }
    m_handles.clear();
    m_loops.clear();
    m_loopFrames.clear();
    m_loopBase.clear();
    m_desired.clear();
    m_paused = false;
    m_dispatched.clear();
    m_resources.clear();
    m_occurrences.clear();
    m_journal.events.clear();
    m_scope.reset();
    m_epoch = 0;
    m_firstTick = m_dispatchFloor = 0;
    m_output = nullptr;
}

void SceneSoundEvents::discard() {
    close();
    m_ledger = Ledger{};
}

void SceneSoundEvents::beginTick(u64 tick) {
    if (m_epoch == 0 || m_recording || m_scope || tick == 0 || tick < m_firstTick) {
        unsupported("sound tick requires an open epoch and no unfinished capture");
    }
    m_tick = tick;
    m_journal.events.clear();
    m_occurrences.clear();
    m_desired.clear();
    ++m_capture;
    m_recording = true;
}

void SceneSoundEvents::abortTick() noexcept {
    m_recording = false;
    m_scope.reset();
    m_journal.events.clear();
    m_occurrences.clear();
    m_desired.clear();
}

SceneSoundEvents::Journal SceneSoundEvents::endTick() {
    if (!m_desired.empty()) {
        unsupported("a sound tick containing desired loops must be ended as a complete frame");
    }
    return endFrame().finite;
}

SceneSoundEvents::Frame SceneSoundEvents::endFrame() {
    if (!m_recording || m_scope) {
        unsupported("sound tick ended with no capture or an outstanding emitter scope");
    }
    m_recording = false;
    return {std::move(m_journal), std::move(m_desired)};
}

SceneSoundResource SceneSoundEvents::bindResource(const SoundSequence& sequence) {
    if (sequence.bank.empty() || !std::isfinite(sequence.volume)) {
        unsupported("recorded sound lacks a native bank/call identity or valid bank gain");
    }
    const SceneSoundResource name{sequence.bank, sequence.call};
    if (const auto resource = m_resources.find(name); resource != m_resources.end()) {
        auto& binding = resource->second;
        if (!sameBinding(binding.sequence, sequence) &&
            !std::ranges::any_of(binding.equivalents, [&](const auto& equivalent) {
                return sameBinding(equivalent, sequence);
            })) {
            if (!sameContent(binding.sequence, sequence)) {
                unsupported(
                    "native sound bank/call changed its immutable resource within an epoch");
            }
            binding.equivalents.push_back(sequence);
        }
    } else {
        m_resources.emplace(name, ResourceBinding{sequence, {}});
    }
    return name;
}

SoundCueToken SceneSoundEvents::nextCue() {
    if (!m_recording || !m_scope) {
        unsupported("gameplay sound without a native tick and explicit emitter/cue scope");
    }
    auto& occurrence = m_occurrences[*m_scope];
    if (occurrence == std::numeric_limits<u32>::max()) {
        unsupported("sound emitter occurrence counter exhausted");
    }
    return {{m_epoch, m_tick, m_scope->first, m_scope->second, occurrence++}};
}

SoundCueToken SceneSoundEvents::beginCue(const SoundSequence& sequence) {
    if (!m_recording || !m_scope) {
        unsupported("owned sound start requires a native tick and stable emitter scope");
    }
    bindResource(sequence);
    return nextCue();
}

void SceneSoundEvents::desireLoop(const FrameLoopKey& key, const SoundSequence& sequence,
                                  SceneSoundPlayback playback, f32 volume, f32 pan,
                                  SoundCueToken start) {
    if (!m_recording || !key.valid() || key.channel >= kMostChannels || !std::isfinite(volume) ||
        !std::isfinite(pan) ||
        (playback != SceneSoundPlayback::RepeatWhileDesired &&
         playback != SceneSoundPlayback::PlayOnceUntilCancelled) ||
        (start != SoundCueToken{} && !validPredecessor(start, m_epoch, m_tick)) ||
        (playback == SceneSoundPlayback::PlayOnceUntilCancelled && !start)) {
        unsupported("invalid desired sound lifetime, identity or parameters");
    }
    if (m_desired.size() == Ledger::kActiveLoops ||
        std::ranges::find(m_desired, key, &DesiredLoop::key) != m_desired.end()) {
        unsupported("duplicate desired sound owner or too many active loops");
    }
    auto resource = bindResource(sequence);
    m_desired.push_back({key, {std::move(resource), volume, pan, {}, playback, start}});
}

SoundCueToken SceneSoundEvents::emit(const SoundSequence& sequence, f32 volume, f32 pan,
                                     SoundCueToken after) {
    if (!m_recording || !m_scope) {
        unsupported("gameplay sound without a native tick and explicit emitter/cue scope");
    }
    if (sequence.bank.empty()) {
        unsupported("recorded sound lacks a native bank/call identity");
    }
    if (sequence.loops()) {
        unsupported("looping sound requires an explicit desired-loop lifecycle");
    }
    const bool hasPredecessor = after != SoundCueToken{};
    if (!std::isfinite(volume) || !std::isfinite(pan) || !std::isfinite(sequence.volume) ||
        (hasPredecessor && !validPredecessor(after, m_epoch, m_tick))) {
        unsupported("invalid sound gain, pan or logical predecessor");
    }
    if (hasPredecessor && after.event.tick == m_tick &&
        std::ranges::find(m_journal.events, after.event, &Ledger::Event::id) ==
            m_journal.events.end()) {
        unsupported("same-tick sound predecessor has not been emitted before its successor");
    }
    if (m_journal.events.size() == Ledger::kEventsPerTick) {
        unsupported("finite sound journal exceeds the per-tick event bound");
    }
    const auto foundOccurrence = m_occurrences.find(*m_scope);
    const auto occurrence = foundOccurrence == m_occurrences.end() ? 0 : foundOccurrence->second;
    if (occurrence == std::numeric_limits<u32>::max()) {
        unsupported("sound emitter occurrence counter exhausted");
    }
    const FrameEventIdentity identity{m_epoch, m_tick, m_scope->first, m_scope->second, occurrence};
    if (after && after.event == identity) {
        unsupported("sound cue cannot depend on itself");
    }
    SceneSoundPayload payload{.resource = bindResource(sequence), .volume = volume, .pan = pan};
    if (hasPredecessor) {
        payload.after = after;
    }
    m_journal.events.push_back({identity, Ledger::Policy::OneShot, std::move(payload), {}});
    m_occurrences.insert_or_assign(*m_scope, occurrence + 1);
    ++m_recorded;
    return {identity};
}

SceneSoundEvents::Result SceneSoundEvents::reconcile(u64 firstTick,
                                                     std::span<const Journal> journals) {
    std::vector<Frame> frames;
    frames.reserve(journals.size());
    for (const auto& journal : journals) {
        frames.push_back({journal, {}});
    }
    return reconcileFrames(firstTick, frames);
}

SceneSoundEvents::Result SceneSoundEvents::reconcileFrames(u64 firstTick,
                                                           std::span<const Frame> frames,
                                                           std::optional<u64> newHead) {
    if (m_recording) {
        unsupported("sound reconciliation before native capture has ended");
    }
    if (firstTick == 0 || firstTick < m_firstTick) {
        unsupported("sound reconciliation predates the loaded native epoch");
    }
    if (frames.size() > Ledger::kHistory ||
        (!frames.empty() && firstTick > std::numeric_limits<u64>::max() - (frames.size() - 1))) {
        unsupported("sound reconciliation exceeds the bounded tick range");
    }
    std::set<FrameEventIdentity> earlier;
    std::vector<Journal> journals;
    journals.reserve(frames.size());
    auto loopFrames = m_loopFrames;
    const auto prior = loopFrames.lower_bound(firstTick);
    auto previous = prior == loopFrames.begin() ? m_loopBase : std::prev(prior)->second;
    loopFrames.erase(prior, loopFrames.end());
    u64 tick = firstTick;
    for (const auto& frame : frames) {
        const auto& journal = frame.finite;
        for (const auto& event : journal.events) {
            if (event.policy != Ledger::Policy::OneShot || event.loop ||
                event.id.cue >= kMostCues || !m_resources.contains(event.payload.resource) ||
                !std::isfinite(event.payload.volume) || !std::isfinite(event.payload.pan)) {
                unsupported("finite sound journal references an unsupported policy or resource");
            }
            if (event.payload.after &&
                (!validPredecessor(*event.payload.after, m_epoch, event.id.tick) ||
                 (event.payload.after->event.tick >= firstTick &&
                  !earlier.contains(event.payload.after->event)))) {
                unsupported(
                    "sound correction refers to a missing or later predecessor in its suffix");
            }
            earlier.insert(event.id);
        }
        journals.push_back(journal);
        auto& complete = journals.back();
        std::set<FrameLoopKey> keys;
        if (frame.loops.size() > Ledger::kActiveLoops) {
            unsupported("complete sound frame exceeds the desired-loop bound");
        }
        for (const auto& desire : frame.loops) {
            const auto& payload = desire.payload;
            if (!desire.key.valid() || desire.key.channel >= kMostChannels ||
                !keys.insert(desire.key).second || !m_resources.contains(payload.resource) ||
                !std::isfinite(payload.volume) || !std::isfinite(payload.pan) || payload.after ||
                (payload.start != SoundCueToken{} &&
                 !validPredecessor(payload.start, m_epoch, tick)) ||
                (payload.playback == SceneSoundPlayback::PlayOnceUntilCancelled &&
                 !validPredecessor(payload.start, m_epoch, tick)) ||
                (payload.playback != SceneSoundPlayback::RepeatWhileDesired &&
                 payload.playback != SceneSoundPlayback::PlayOnceUntilCancelled)) {
                unsupported("invalid complete desired-loop frame");
            }
            complete.events.push_back({{m_epoch, tick, desire.key.emitter,
                                        kLoopStartTag | desire.key.channel, desire.key.instance},
                                       Ledger::Policy::LoopStart,
                                       payload,
                                       desire.key});
        }
        for (const auto& old : previous) {
            if (!keys.contains(old.key)) {
                // A correction may change this tick's start to a stop. They are
                // distinct semantic events, while the retained owner key stays
                // the same; never change an existing event identity's policy.
                complete.events.push_back({{m_epoch, tick, old.key.emitter,
                                            kLoopStopTag | old.key.channel, old.key.instance},
                                           Ledger::Policy::LoopStop,
                                           old.payload,
                                           old.key});
            }
        }
        previous = frame.loops;
        loopFrames.emplace(tick++, previous);
    }
    auto result = newHead ? m_ledger.replaceSuffix(m_epoch, firstTick, journals, *newHead)
                          : m_ledger.replace(m_epoch, firstTick, journals);
    if (result) {
        m_loopFrames.swap(loopFrames);
        while (m_loopFrames.size() > Ledger::kHistory) {
            m_loopBase = std::move(m_loopFrames.begin()->second);
            m_loopFrames.erase(m_loopFrames.begin());
        }
        const auto head = m_ledger.head().value_or(m_firstTick);
        m_dispatchFloor = std::max(
            m_dispatchFloor, head >= Ledger::kHistory ? head - Ledger::kHistory + 1 : m_firstTick);
        std::erase_if(m_dispatched, [this](const auto& id) { return id.tick < m_dispatchFloor; });
    }
    return result;
}

SceneSoundEvents::Result SceneSoundEvents::confirm(u64 allSeatsThrough) {
    if (m_recording) {
        unsupported("sound confirmation before native capture has ended");
    }
    return m_ledger.confirm(m_epoch, allSeatsThrough);
}

void SceneSoundEvents::dispatch(const Result& result) {
    if (m_recording || !result) {
        unsupported("sound output requires a successful completed reconciliation");
    }
    const auto& desired = m_loopFrames.empty() ? m_loopBase : m_loopFrames.rbegin()->second;
    for (const auto& action : result.actions) {
        const bool finite = action.kind == Ledger::ActionKind::Present;
        if ((action.kind != Ledger::ActionKind::Present &&
             action.kind != Ledger::ActionKind::StartLoop &&
             action.kind != Ledger::ActionKind::UpdateLoop &&
             action.kind != Ledger::ActionKind::StopLoop) ||
            action.event.id.epoch != m_epoch || !action.event.id.valid() ||
            (finite && action.event.id.tick < m_dispatchFloor) ||
            (finite ? action.event.loop.has_value() : !action.event.loop.has_value()) ||
            !m_resources.contains(action.event.payload.resource)) {
            unsupported("sound output delta does not belong to the current finite-cue epoch");
        }
        if (!finite) {
            const auto current = std::ranges::find(desired, *action.event.loop, &DesiredLoop::key);
            if (action.kind == Ledger::ActionKind::StopLoop
                    ? current != desired.end()
                    : current == desired.end() || current->payload != action.event.payload) {
                unsupported("sound loop delta no longer describes the reconciled final state");
            }
        }
    }
    if (m_output) {
        std::erase_if(m_handles,
                      [this](const auto& voice) { return !m_output->isPlaying(voice.second); });
    }
    for (const auto& action : result.actions) {
        const auto& event = action.event;
        if (action.kind != Ledger::ActionKind::Present) {
            if (action.kind == Ledger::ActionKind::StopLoop) {
                if (const auto found = m_loops.find(*event.loop); found != m_loops.end()) {
                    if (m_output) {
                        m_output->stop(found->second.handle);
                    }
                    m_loops.erase(found);
                }
            } else {
                startLoop(*event.loop, event.payload);
            }
            continue;
        }
        if (!m_dispatched.insert(event.id).second || m_output == nullptr) {
            continue;
        }
        const auto& payload = event.payload;
        SoundHandle after = kNoSound;
        if (payload.after) {
            if (const auto previous = m_handles.find(payload.after->event);
                previous != m_handles.end()) {
                after = previous->second;
            }
        }
        try {
            const auto handle =
                m_output->playAfter(after, m_resources.at(payload.resource).sequence,
                                    payload.volume, SoundCategory::Effects);
            if (handle != kNoSound) {
                m_handles[event.id] = handle;
                m_output->setPan(handle, payload.pan);
            }
        } catch (const std::exception& error) {
            // Admission and the logical chain are already fixed. Optional audio
            // failure must not alter gameplay or replay a cue on the next tick.
            log::warn("Scene sound output: {}", error.what());
        }
    }
    servicePlayback();
}

void SceneSoundEvents::startLoop(const FrameLoopKey& key, const SceneSoundPayload& payload) {
    const auto found = m_loops.find(key);
    if (found != m_loops.end()) {
        auto& old = found->second;
        if (old.payload.resource == payload.resource && old.payload.playback == payload.playback &&
            old.payload.start == payload.start) {
            old.payload = payload;
            if (m_output && old.handle != kNoSound) {
                m_output->setVolume(old.handle, payload.volume);
                m_output->setPan(old.handle, payload.pan);
            }
            return;
        }
        if (m_output) {
            m_output->stop(old.handle);
        }
    }
    m_loops.insert_or_assign(key, PlayingLoop{payload});
}

void SceneSoundEvents::servicePlayback() {
    if (m_recording) {
        unsupported("servicing desired sound playback during a native tick");
    }
    if (m_output == nullptr || m_paused) {
        return;
    }
    for (auto& [key, loop] : m_loops) {
        if (loop.handle != kNoSound && m_output->isPlaying(loop.handle)) {
            continue;
        }
        if (loop.handle != kNoSound) {
            loop.finished = true;
            loop.handle = kNoSound;
        }
        if (loop.finished && loop.payload.playback == SceneSoundPlayback::PlayOnceUntilCancelled) {
            continue;
        }
        try {
            loop.handle = m_output->play(m_resources.at(loop.payload.resource).sequence,
                                         loop.payload.volume, SoundCategory::Effects);
            m_output->setPan(loop.handle, loop.payload.pan);
            if (loop.handle == kNoSound) {
                loop.finished = true;
            }
        } catch (const std::exception& error) {
            loop.finished = true;
            log::warn("Desired scene sound: {}", error.what());
        }
    }
}

void SceneSoundEvents::pausePlayback() {
    if (m_recording) {
        unsupported("pausing desired sound playback during a native tick");
    }
    m_paused = true;
    if (m_output) {
        for (auto& [key, loop] : m_loops) {
            if (loop.handle != kNoSound) {
                loop.finished = !m_output->isPlaying(loop.handle);
                m_output->stop(loop.handle);
                loop.handle = kNoSound;
            }
        }
    }
}

void SceneSoundEvents::resumePlayback() {
    if (m_recording) {
        unsupported("resuming desired sound playback during a native tick");
    }
    m_paused = false;
    servicePlayback();
}

} // namespace gdl::game
