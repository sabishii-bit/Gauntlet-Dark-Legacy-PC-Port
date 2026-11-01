#include "game/rollback/NativeRollbackDriver.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <map>
#include <utility>

#include "engine/io/NativeStateDigest.h"

#include "game/netplay/CharacterProfile.h"
#include "game/rollback/LocalStateHistory.h"

namespace gdl::game {

struct NativeRollbackDriver::Load {
    // Identity only: a released scene is never dereferenced through this record.
    const PlayScene* scene = nullptr;
    u64 generation = 0;
    bool failed = false;
};

struct NativeRollbackDriver::Data {
    struct State {
        PlayScene::LocalSnapshot scene;
        Control control;
    };
    PlayScene* scene = nullptr;
    std::shared_ptr<Load> load;
    MatchContext context;
    u8 localPeer = 0;
    AudioMode audioMode = AudioMode::Disabled;
    RollbackInputs inputs;
    LocalStateHistory<State> history;
    PlayScene::LocalSnapshot binding;
    Ledger ledger;
    Control control;
    std::vector<InputCommand> pending;
    // Only digest-period and terminal post-tick states are retained here, not
    // another every-frame history. Correction discards the wrong suffix.
    std::map<u64, State> hashes;
    std::map<u64, std::vector<PlayScene::ConsoleReply>> consoleReplies;
    Statistics stats;
    EpochInputJournal journal;
    std::optional<u64> rebuildThrough;
    Failure failed = Failure::None;
    u64 confirmed = 0;
    bool terminalReported = false;

    bool rosterMatches() const {
        if (scene == nullptr || !scene->isOpen() || scene->simulationTickRate() != 60) {
            return false;
        }
        std::array<bool, InputCommand::kSeats> seen{};
        for (const auto& player : scene->participants()) {
            const s32 seat = player.actor.player();
            if (seat < 0 || seat >= static_cast<s32>(seen.size()) ||
                seen[static_cast<usize>(seat)] || context.owners[static_cast<usize>(seat)] == 0) {
                return false;
            }
            seen[static_cast<usize>(seat)] = true;
        }
        for (usize seat = 0; seat < seen.size(); ++seat) {
            if (seen[seat] != (context.owners[seat] != 0)) {
                return false;
            }
        }
        return true;
    }
    bool fail(Failure reason) {
        if (failed == Failure::None) {
            failed = reason;
        }
        if (load) {
            load->failed = true;
        }
        return false;
    }
    bool valid() {
        const u64 audioEpoch = scene != nullptr ? scene->recordedAudioEpoch() : 0;
        const bool audioBound =
            audioEpoch == (audioMode == AudioMode::Required ? context.epoch : 0);
        return failed == Failure::None && rosterMatches() && audioBound && scene != nullptr &&
                       scene->canRestoreLocal(binding)
                   ? true
                   : fail(Failure::Binding);
    }
    bool admitPending() {
        if (pending.empty() || control.terminal || terminalReported || !inputs.sampleReady()) {
            return true;
        }
        if (!inputs.sample(pending)) {
            return fail(Failure::Input);
        }
        pending.clear();
        return true;
    }
    void filterArrival(SessionInputs::Frame& frame, u64 tick) {
        const auto& camera = scene->arrival().camera();
        if (!camera.active()) {
            control.arrivalVotes = {};
            return;
        }
        const bool eligible = camera.mode() == StartCamera::Mode::Legacy &&
                              camera.phase() == StartCamera::Phase::Hold &&
                              camera.ticksLeft() <= StartCamera::kSkipBelow;
        // A dropped peer's seat never holds up the skip, as with the entry movie's vote.
        const auto voting = [&](usize seat) {
            return context.owners[seat] != 0 && !inputs.suspendedSeat(seat, tick);
        };
        bool unanimous = eligible;
        for (usize seat = 0; seat < frame.size(); ++seat) {
            auto& menu = frame[seat].menu;
            if (eligible && voting(seat)) {
                control.arrivalVotes[seat] =
                    control.arrivalVotes[seat] || menu.start || menu.select || menu.back;
                unanimous = unanimous && control.arrivalVotes[seat];
            }
            menu.start = menu.select = menu.back = false;
        }
        if (unanimous) {
            for (usize seat = 0; seat < frame.size(); ++seat) {
                frame[seat].menu.select = voting(seat);
            }
        }
    }
    bool step(Result& result, bool replay) {
        const u64 tick = inputs.tick();
        // A rebuild keeps history only where a correction could still reach.
        const bool keepHistory =
            !rebuildThrough || tick + LocalStateHistory<State>::kCapacity > *rebuildThrough;
        if (control.terminal || (keepHistory && !history.capture(tick, [&](State& saved) {
                if (!scene->saveLocal(saved.scene)) {
                    return false;
                }
                saved.control = control;
                return true;
            }))) {
            return fail(Failure::Snapshot);
        }
        const auto frame = inputs.advance();
        if (!frame) {
            return fail(Failure::Input);
        }
        // A dropped peer's characters leave play at its first still tick on every peer,
        // however late the drop reached this one: suspension runs the ticks after it again.
        for (usize seat = 0; seat < context.owners.size(); ++seat) {
            if (inputs.suspendedSeat(seat, tick)) {
                scene->waitInTower(static_cast<s32>(seat));
            }
        }
        SessionInputs::Frame commands;
        std::vector<ConsoleAction> console;
        for (usize seat = 0; seat < commands.size(); ++seat) {
            commands[seat] = SessionInputs::playInput(frame->commands[seat]);
            if (frame->commands[seat].console) {
                console.push_back(*frame->commands[seat].console);
            }
        }
        filterArrival(commands, tick);
        auto output = scene->recordStep(context.epoch, tick, 1.0 / 60, commands, console);
        if (!output.consoleReplies.empty() && (!rebuildThrough || tick > *rebuildThrough)) {
            consoleReplies[tick] = std::move(output.consoleReplies);
        }
        if (output.audio.has_value() != (audioMode == AudioMode::Required)) {
            return fail(Failure::Audio);
        }
        if (!rosterMatches()) {
            return fail(Failure::Binding);
        }
        if (output.outcome != PlayOutcome::GameOver &&
            std::ranges::any_of(scene->participants(), &PlayerRuntime::departed)) {
            output.outcome = PlayOutcome::Leave;
            if (output.audio) {
                // The scene gathered desires while its outcome was Running.
                // A roster departure is this driver's terminal boundary.
                output.audio->loops.clear();
            }
        }
        if (output.outcome != PlayOutcome::Running) {
            control.terminal = Terminal{tick, output.outcome};
        }
        // Agreement restarts after the resync point; its own tick was settled by the host.
        const bool observed = !rebuildThrough || tick > *rebuildThrough;
        if ((tick % kDigestPeriod == 0 && observed) || control.terminal) {
            if (hashes.size() >= Ledger::kHistory) {
                return fail(Failure::Observation);
            }
            auto& saved = hashes[tick];
            if (!scene->saveLocal(saved.scene)) {
                return fail(Failure::Snapshot);
            }
            saved.control = control;
        }
        result.frames.push_back(
            {tick, *frame, std::move(output.journal), replay, std::move(output.audio)});
        ++stats.nativeTicks;
        stats.replayTicks += replay ? 1 : 0;
        return true;
    }
    bool correct(Result& result) {
        const auto wrong = inputs.mismatchTick();
        if (!wrong) {
            return true;
        }
        const u64 previousHead = inputs.tick();
        if (*wrong <= confirmed || previousHead - *wrong > RollbackInputs::kPredictionTicks) {
            return fail(Failure::Replay);
        }
        const auto* saved = history.find(*wrong);
        if (saved == nullptr || !scene->restoreLocal(saved->scene)) {
            return fail(Failure::Snapshot);
        }
        control = saved->control;
        history.discardAfter(*wrong);
        hashes.erase(hashes.lower_bound(*wrong), hashes.end());
        consoleReplies.erase(consoleReplies.lower_bound(*wrong), consoleReplies.end());
        if (!inputs.rewind(*wrong)) {
            return fail(Failure::Replay);
        }
        ++stats.corrections;
        stats.maxReplay = std::max(stats.maxReplay, previousHead - *wrong);
        while (inputs.tick() < previousHead && !control.terminal) {
            if (!inputs.ready() || !step(result, true)) {
                return fail(Failure::Replay);
            }
        }
        if (control.terminal && inputs.replaying() && !inputs.finishReplayAt(inputs.tick())) {
            return fail(Failure::Replay);
        }
        return true;
    }
    static void append(Result& result, Ledger::Result output) {
        result.actions.insert(result.actions.end(), std::make_move_iterator(output.actions.begin()),
                              std::make_move_iterator(output.actions.end()));
        result.unretractable.insert(result.unretractable.end(), output.unretractable.begin(),
                                    output.unretractable.end());
    }
    bool publish(Result& result) {
        SceneSoundEvents::Result audioOutput;
        const auto appendAudio = [&](SceneSoundEvents::Result output) {
            if (!output) {
                return fail(Failure::Audio);
            }
            audioOutput.actions.insert(audioOutput.actions.end(),
                                       std::make_move_iterator(output.actions.begin()),
                                       std::make_move_iterator(output.actions.end()));
            audioOutput.unretractable.insert(audioOutput.unretractable.end(),
                                             output.unretractable.begin(),
                                             output.unretractable.end());
            return true;
        };
        if (!result.frames.empty()) {
            std::vector<Ledger::Journal> journals;
            journals.reserve(result.frames.size());
            for (const auto& frame : result.frames) {
                journals.push_back(frame.journal);
            }
            auto output = ledger.replaceSuffix(context.epoch, result.frames.front().tick, journals,
                                               inputs.tick() - 1);
            if (!output) {
                return fail(Failure::Journal);
            }
            append(result, std::move(output));
            if (audioMode == AudioMode::Required) {
                std::vector<SceneSoundEvents::Frame> frames;
                frames.reserve(result.frames.size());
                for (const auto& frame : result.frames) {
                    if (!frame.audio) {
                        return fail(Failure::Audio);
                    }
                    frames.push_back(*frame.audio);
                }
                if (!appendAudio(scene->reconcileRecordedAudio(result.frames.front().tick, frames,
                                                               inputs.tick() - 1))) {
                    return false;
                }
            }
        }
        const u64 through = inputs.confirmedTick();
        if (through > confirmed) {
            auto output = ledger.confirm(context.epoch, through);
            if (!output) {
                return fail(Failure::Journal);
            }
            append(result, std::move(output));
            if (audioMode == AudioMode::Required &&
                !appendAudio(scene->confirmRecordedAudio(through))) {
                return false;
            }
            for (auto hash = hashes.begin(); hash != hashes.end() && hash->first <= through;) {
                const auto sceneDigest = scene->nativeLocalDigest(hash->second.scene);
                if (!sceneDigest) {
                    return fail(Failure::Observation);
                }
                const ConfirmedDigest value{hash->first,
                                            agreementDigest(*sceneDigest, hash->second.control)};
                if (hash->first % kDigestPeriod == 0) {
                    result.digests.push_back(value);
                }
                if (hash->second.control.terminal &&
                    hash->second.control.terminal->tick == hash->first) {
                    result.terminalDigest = value;
                }
                hash = hashes.erase(hash);
            }
            for (u64 tick = confirmed + 1; tick <= through; ++tick) {
                const auto frame = inputs.actualFrame(tick);
                // A journal that has reached its half-hour simply stops growing.
                if (!frame || (!journal.append(tick, *frame) &&
                               journal.through() < EpochInputJournal::kMaxFrames)) {
                    return fail(Failure::Journal);
                }
            }
            confirmed = through;
            for (auto reply = consoleReplies.begin();
                 reply != consoleReplies.end() && reply->first <= through;) {
                result.consoleReplies.insert(result.consoleReplies.end(),
                                               std::make_move_iterator(reply->second.begin()),
                                               std::make_move_iterator(reply->second.end()));
                reply = consoleReplies.erase(reply);
            }
        }
        if (!terminalReported && control.terminal && control.terminal->tick <= confirmed) {
            if (!result.terminalDigest || result.terminalDigest->tick != control.terminal->tick) {
                return fail(Failure::Observation);
            }
            switch (control.terminal->outcome) {
            case PlayOutcome::Travel: result.status = Status::Travel; break;
            case PlayOutcome::GameOver: result.status = Status::GameOver; break;
            case PlayOutcome::Leave: result.status = Status::Leave; break;
            case PlayOutcome::Running: return fail(Failure::Replay);
            }
            terminalReported = true;
        }
        if (audioMode == AudioMode::Required) {
            result.audio = std::move(audioOutput);
        }
        return true;
    }
};

NativeRollbackDriver::NativeRollbackDriver() : m_data(std::make_unique<Data>()) {}
NativeRollbackDriver::~NativeRollbackDriver() = default;

SimulationObservation::Digest
NativeRollbackDriver::agreementDigest(const SimulationObservation::Digest& scene,
                                      const Control& control) {
    const auto& [arrivalVotes, terminal] = control;
    NativeStateDigest digest;
    digest.string("NativeRollbackDriver.confirmed.1");
    digest.values(scene, arrivalVotes);
    digest.flag(terminal.has_value());
    if (terminal) {
        digest.fields<2>(*terminal);
    }
    return digest.finish();
}

bool NativeRollbackDriver::bind(PlayScene& scene, const MatchContext& context, u8 localPeer,
                                AudioMode audio) {
    if (!context.valid() || context.transition == MatchTransition::Shop || context.tickRate != 60 ||
        context.inputLead > RollbackInputs::kMaxDelay || context.epoch < m_latestEpoch ||
        (context.epoch == m_latestEpoch && !m_rebinding) ||
        scene.recordedAudioEpoch() != (audio == AudioMode::Required ? context.epoch : 0)) {
        return false;
    }
    const auto existing = std::ranges::find_if(m_loads, [&](const auto& load) {
        return load->scene == &scene && load->generation == scene.loadGeneration();
    });
    if (existing != m_loads.end() && (*existing)->failed) {
        return false;
    }
    auto candidate = std::make_unique<Data>();
    candidate->scene = &scene;
    candidate->load = existing != m_loads.end()
                          ? *existing
                          : std::make_shared<Load>(Load{&scene, scene.loadGeneration(), false});
    candidate->context = context;
    candidate->localPeer = localPeer;
    candidate->audioMode = audio;
    if (!candidate->rosterMatches() ||
        // Keep negotiated profile admission at the epoch boundary. The native
        // digest hashes retained values as-is; it must not normalize or validate
        // gameplay through a transport projection on every tick.
        std::ranges::any_of(scene.participants(),
                            [](const PlayerRuntime& player) {
                                return player.departed ||
                                       !CharacterProfile::capture(player.actor.save());
                            }) ||
        !candidate->inputs.open(localPeer, context.epoch, context.owners, context.grants,
                                context.inputLead) ||
        !scene.saveLocal(candidate->binding) || !scene.nativeAgreementDigest() ||
        !candidate->ledger.reset(context.epoch) ||
        !candidate->journal.open(context.epoch, context.owners)) {
        return false;
    }
    if (existing == m_loads.end()) {
        m_loads.push_back(candidate->load);
    }
    m_data = std::move(candidate);
    m_latestEpoch = context.epoch;
    return true;
}
bool NativeRollbackDriver::rebind(PlayScene& scene, const MatchContext& context, u8 localPeer,
                                  AudioMode audio) {
    if (context.epoch != m_latestEpoch) {
        return false;
    }
    m_rebinding = true;
    const bool bound = bind(scene, context, localPeer, audio);
    m_rebinding = false;
    return bound;
}
bool NativeRollbackDriver::restoreBinding() {
    const auto& data = *m_data;
    if (data.failed != Failure::None || data.scene == nullptr ||
        !data.scene->restoreLocal(data.binding)) {
        return false;
    }
    clear();
    return true;
}
const EpochInputJournal& NativeRollbackDriver::journal() const {
    return m_data->journal;
}
std::vector<InputCommand> NativeRollbackDriver::actualInputsAfter(u64 tick) const {
    return m_data->inputs.actualAfter(tick);
}
bool NativeRollbackDriver::adopt(std::span<const InputCommand> commands) {
    auto& data = *m_data;
    if (data.failed != Failure::None || commands.empty()) {
        return false;
    }
    return std::ranges::all_of(
        commands, [&](const InputCommand& command) { return data.inputs.adoptActual(command); });
}
bool NativeRollbackDriver::suspend(u8 peer, u64 fromTick) {
    auto& data = *m_data;
    return data.failed == Failure::None && data.inputs.suspend(peer, fromTick, data.confirmed);
}
bool NativeRollbackDriver::observeHostReceipt(u8 peer, u64 through) {
    auto& data = *m_data;
    return data.failed == Failure::None && data.inputs.observeHostReceipt(peer, through);
}
bool NativeRollbackDriver::beginRebuild(u64 through) {
    auto& data = *m_data;
    if (!data.valid() || data.rebuildThrough || through == 0 || data.inputs.tick() != 1) {
        return false;
    }
    data.rebuildThrough = through;
    return true;
}
bool NativeRollbackDriver::finishRebuild() {
    auto& data = *m_data;
    if (!data.rebuildThrough || data.failed != Failure::None ||
        data.inputs.tick() != *data.rebuildThrough + 1 || !data.inputs.resumeSampling()) {
        return false;
    }
    data.rebuildThrough.reset();
    return true;
}
bool NativeRollbackDriver::rebuilding() const {
    return m_data->rebuildThrough.has_value();
}
void NativeRollbackDriver::clear() {
    m_data = std::make_unique<Data>();
}

bool NativeRollbackDriver::sample(const SessionInputs::Frame& local,
                                  std::optional<ConsoleAction> console) {
    auto& data = *m_data;
    if (!data.valid()) {
        return false;
    }
    if (data.terminalReported) {
        return true;
    }
    if (console && (data.localPeer != 1 || !console->valid() ||
                    !data.inputs.sampleReady() || data.control.terminal)) {
        return false;
    }
    std::vector<InputCommand> commands;
    for (usize seat = 0; seat < data.context.owners.size(); ++seat) {
        if (data.context.owners[seat] != data.localPeer) {
            continue;
        }
        const auto captured =
            data.scene->captureNativeInput(static_cast<s32>(seat), local[commands.size()]);
        // Coarse analogue steps keep a steady hand's commands identical from tick to tick,
        // so peers predicting it by holding its last input are right far more often.
        commands.push_back(SessionInputs::command(captured, data.context.epoch,
                                                  data.inputs.sampleTick(),
                                                  data.context.grants[seat], static_cast<u8>(seat))
                               .steadied());
    }
    if (commands.empty() || (!data.pending.empty() && commands.size() != data.pending.size())) {
        return data.fail(Failure::Input);
    }
    for (usize index = 0; index < commands.size(); ++index) {
        auto& command = commands[index];
        if (!command.valid()) {
            return data.fail(Failure::Input);
        }
        if (!data.pending.empty()) {
            const auto& pending = data.pending[index];
            command.console = pending.console;
            command.pressedButtons |= pending.pressedButtons;
            if (data.scene->hints().acceptsTopic(pending.hintTopic, pending.hintRevision)) {
                command.hintTopic = pending.hintTopic;
                command.hintRevision = pending.hintRevision;
                command.pressedButtons &= ~static_cast<u32>(CommandPress::MenuAny);
            }
            if (pending.pressed(CommandPress::Attack)) {
                command.heldButtons |= static_cast<u32>(CommandHeld::Attack);
            }
        }
    }
    if (console) {
        // The host is the sole admitting writer. The target is still the requesting
        // player's canonical seat, not the host seat carrying this one-shot event.
        commands.front().console = std::move(console);
    }
    data.pending = std::move(commands);
    // A tentative exit may disappear under an earlier correction. Preserve
    // physical taps meanwhile, but do not admit future simulation input while
    // held at that speculative boundary.
    return data.admitPending();
}
bool NativeRollbackDriver::clearPendingInput(u8 canonicalSeat) {
    auto& data = *m_data;
    if (data.scene == nullptr || data.failed != Failure::None || data.localPeer == 0 ||
        canonicalSeat >= data.context.owners.size() ||
        data.context.owners[canonicalSeat] != data.localPeer || !data.valid()) {
        return false;
    }
    for (auto& command : data.pending) {
        if (command.seat == canonicalSeat) {
            auto console = std::move(command.console);
            command = SessionInputs::command({}, command.epoch, command.tick, command.grant,
                                             command.seat);
            command.console = std::move(console);
        }
    }
    return true;
}
NativeRollbackDriver::Admission
NativeRollbackDriver::submit(u8 authenticatedPeer, std::span<const InputCommand> commands) {
    auto& data = *m_data;
    if (data.failed != Failure::None) {
        return Admission::Invalid;
    }
    const auto result = data.inputs.submit(authenticatedPeer, commands);
    if (result == Admission::Conflict) {
        data.fail(Failure::Input);
    }
    return result;
}
NativeRollbackDriver::Admission NativeRollbackDriver::observePeerTick(u8 authenticatedPeer,
                                                                      u64 epoch, u64 nextTick,
                                                                      u8 transitTicks) {
    return m_data->failed == Failure::None
               ? m_data->inputs.observePeerTick(authenticatedPeer, epoch, nextTick, transitTicks)
               : Admission::Invalid;
}
NativeRollbackDriver::Result NativeRollbackDriver::advance(bool forward) {
    auto& data = *m_data;
    Result result;
    try {
        if (!data.valid() || !data.correct(result)) {
            result.status = Status::Failed;
            return result;
        }
        // Shipping captures devices before correction. A pending mismatch may
        // have blocked that capture's admission; once replay finishes, admit
        // the same captured sample at its original sampleTick without another
        // device poll or an avoidable extra application frame of latency.
        if (forward && !data.admitPending()) {
            result.status = Status::Failed;
            return result;
        }
        if (forward && !data.control.terminal && !data.terminalReported) {
            if (!data.rebuildThrough && data.inputs.timeSyncIdle()) {
                ++data.stats.pacingIdles;
            } else if (!data.inputs.ready()) {
                ++data.stats.stalls;
            } else if (!data.step(result, false)) {
                result.status = Status::Failed;
                return result;
            } else {
                result.status = Status::Advanced;
            }
        }
        if (data.publish(result)) {
            return result;
        }
    } catch (const std::exception&) {
        data.fail(Failure::Replay);
    }
    result.status = Status::Failed;
    result.actions.clear();
    result.digests.clear();
    result.terminalDigest.reset();
    result.unretractable.clear();
    result.audio.reset();
    return result;
}

bool NativeRollbackDriver::sampleReady() const {
    return m_data->failed == Failure::None && !m_data->control.terminal &&
           !m_data->terminalReported && m_data->inputs.sampleReady();
}
u64 NativeRollbackDriver::sampleTick() const {
    return m_data->inputs.sampleTick();
}
u64 NativeRollbackDriver::nextTick() const {
    return m_data->inputs.tick();
}
u64 NativeRollbackDriver::confirmedTick() const {
    return m_data->confirmed;
}
std::optional<u64> NativeRollbackDriver::mismatchTick() const {
    return m_data->inputs.mismatchTick();
}
std::optional<NativeRollbackDriver::Terminal> NativeRollbackDriver::tentativeTerminal() const {
    return m_data->control.terminal;
}
std::array<bool, InputCommand::kSeats> NativeRollbackDriver::arrivalVotes() const {
    return m_data->control.arrivalVotes;
}
std::span<const InputCommand> NativeRollbackDriver::redundantInputs() const {
    return m_data->inputs.redundantInputs();
}
const NativeRollbackDriver::Statistics& NativeRollbackDriver::statistics() const {
    return m_data->stats;
}
NativeRollbackDriver::Failure NativeRollbackDriver::failure() const {
    return m_data->failed;
}
} // namespace gdl::game
