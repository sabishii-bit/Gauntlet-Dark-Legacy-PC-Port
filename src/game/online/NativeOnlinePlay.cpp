#include "game/online/NativeOnlinePlay.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <set>
#include <span>
#include <utility>

#include "engine/core/Log.h"

#include "game/netplay/OnlineParty.h"
#include "game/scenes/AfterLevelScene.h"

namespace {

/** The seats this machine plays, a bit each, for the scene's local presentation. */
gdl::u8 seatMask(std::span<const gdl::u8> seats) {
    gdl::game::SeatSet mask;
    for (const gdl::u8 seat : seats) {
        mask.insert(seat);
    }
    return mask.bits();
}

/** The lowest seat this machine plays, whose camera it looks through under solo cameras. */
gdl::s32 lowestSeat(std::span<const gdl::u8> seats) {
    gdl::s32 lowest = -1;
    for (const gdl::u8 seat : seats) {
        if (lowest < 0 || seat < lowest) {
            lowest = seat;
        }
    }
    return lowest;
}

} // namespace

namespace gdl::game {
namespace {
std::string_view failureDescription(NativeOnlinePlay::Failure failure) {
    switch (failure) {
    case NativeOnlinePlay::Failure::None: return {};
    case NativeOnlinePlay::Failure::Session: return "Online session ended";
    case NativeOnlinePlay::Failure::Scene: return "Native simulation could not safely continue";
    case NativeOnlinePlay::Failure::Assets: return "Required stage assets could not be loaded";
    case NativeOnlinePlay::Failure::Input: return "Online input could not be admitted";
    case NativeOnlinePlay::Failure::Presentation: return "Online transition could not be presented";
    case NativeOnlinePlay::Failure::Party: return "Online character progress disagrees";
    }
    return "Online scene stopped";
}
std::string_view inputAdmissionDescription(RollbackInputs::Admission result) {
    switch (result) {
    case RollbackInputs::Admission::Accepted: return "accepted";
    case RollbackInputs::Admission::Duplicate: return "duplicate";
    case RollbackInputs::Admission::Stale: return "stale";
    case RollbackInputs::Admission::Invalid: return "invalid";
    case RollbackInputs::Admission::WrongPeer: return "wrong peer";
    case RollbackInputs::Admission::WrongEpoch: return "wrong epoch";
    case RollbackInputs::Admission::WrongGrant: return "wrong seat grant";
    case RollbackInputs::Admission::TooEarly: return "ahead of input window";
    case RollbackInputs::Admission::Conflict: return "conflicting input";
    }
    return "unknown";
}

std::optional<MatchParty> profilesOf(std::span<const PartyMember> members,
                                     const MatchOwners& owners) {
    MatchParty profiles;
    for (const auto& member : members) {
        if (member.player < 0 || member.player >= static_cast<s32>(profiles.size())) {
            return std::nullopt;
        }
        const auto seat = static_cast<usize>(member.player);
        if (owners[seat] == 0 || profiles[seat]) {
            return std::nullopt;
        }
        profiles[seat] = CharacterProfile::capture(member.save);
        if (!profiles[seat]) {
            return std::nullopt;
        }
    }
    for (usize seat = 0; seat < profiles.size(); ++seat) {
        if (profiles[seat].has_value() != (owners[seat] != 0)) {
            return std::nullopt;
        }
    }
    return profiles;
}

bool sameProfiles(const MatchParty& local, const MatchParty& received) {
    for (usize seat = 0; seat < local.size(); ++seat) {
        if (local[seat].has_value() != received[seat].has_value()) {
            return false;
        }
        if (local[seat]) {
            const auto a = CharacterProfilePacket::encode(*local[seat]);
            const auto b = CharacterProfilePacket::encode(*received[seat]);
            if (!a || !b || *a != *b) {
                return false;
            }
        }
    }
    return true;
}

bool sameShop(const ShopSetup& a, const ShopSetup& b) {
    return a.source == b.source && a.destination == b.destination && a.expanded == b.expanded &&
           a.fallen == b.fallen && a.maxima == b.maxima && a.totals == b.totals &&
           a.ending == b.ending;
}
} // namespace

struct NativeOnlinePlay::Data {
    struct Level {
        LevelWorld world;
        PlayScene scene;
        NativeRollbackDriver driver;
        MatchSession* match = nullptr;
        u64 boundEpoch = 0;

        void unbind() {
            if (match != nullptr && boundEpoch != 0) {
                match->detachNativeInputs(boundEpoch);
            }
            match = nullptr;
            boundEpoch = 0;
            driver.clear();
            scene.endRecordedAudio();
        }
        Level() = default;
        Level(const Level&) = delete;
        Level& operator=(const Level&) = delete;
        ~Level() {
            unbind();
            scene.close();
        }
        bool bind(MatchSession& session) { return bindDriver(session) && attach(session); }
        bool bindDriver(MatchSession& session) {
            const auto& agreed = session.context();
            return scene.beginRecordedAudio(agreed.epoch) &&
                   driver.bind(scene, agreed, session.localPeer(),
                               NativeRollbackDriver::AudioMode::Required);
        }
        /** Binds the scene's own epoch again once its driver returned it to the epoch's
         * start, for a rebuild that cannot load the stage afresh. */
        bool rebindDriver(MatchSession& session) {
            const auto& agreed = session.context();
            return scene.restartRecordedAudio(agreed.epoch) &&
                   driver.rebind(scene, agreed, session.localPeer(),
                                 NativeRollbackDriver::AudioMode::Required);
        }
        /** Routes the room's input to the driver; a rebuilding scene attaches only once
         * its head stands near the room's, so nothing arrives too early to admit. */
        bool attach(MatchSession& session) {
            const auto& agreed = session.context();
            if (!session.attachNativeInputs(
                    agreed.epoch,
                    [this](const RollbackExchange::Message& message, std::optional<u8> transit) {
                        const auto admitted = [](RollbackInputs::Admission result) {
                            return result == RollbackInputs::Admission::Accepted ||
                                   result == RollbackInputs::Admission::Duplicate ||
                                   result == RollbackInputs::Admission::Stale;
                        };
                        if (!message.commands.empty()) {
                            const auto result = driver.submit(message.origin, message.commands);
                            if (!admitted(result)) {
                                log::error("Native input refused: reason={} epoch={} origin={} "
                                           "next={} confirmed={} sender_next={} commands={}",
                                           inputAdmissionDescription(result), message.epoch,
                                           message.origin, driver.nextTick(),
                                           driver.confirmedTick(), message.nextTick,
                                           message.commands.size());
                                return false;
                            }
                        }
                        if (transit) {
                            const auto result = driver.observePeerTick(
                                message.origin, message.epoch, message.nextTick, *transit);
                            if (!admitted(result)) {
                                log::error("Native progress refused: reason={} epoch={} origin={} "
                                           "next={} confirmed={} sender_next={} transit_ticks={}",
                                           inputAdmissionDescription(result), message.epoch,
                                           message.origin, driver.nextTick(),
                                           driver.confirmedTick(), message.nextTick, *transit);
                                return false;
                            }
                        }
                        return true;
                    })) {
                return false;
            }
            match = &session;
            boundEpoch = agreed.epoch;
            return true;
        }
    };
    struct Boundary {
        u64 sourceEpoch = 0;
        u64 sourceTick = 0;
        u32 scene = 0;
        MatchTransition transition = MatchTransition::Travel;
        bool consoleTravel = false;
        bool challengeWon = false;
        MatchParty profiles;
        std::vector<PartyMember> members;
        std::optional<ShopSetup> shop;

        bool matches(const MatchContext& next) const {
            return next.epoch == sourceEpoch + 1 && next.sourceEpoch == sourceEpoch &&
                   next.sourceTick == sourceTick && next.scene == scene &&
                   next.transition == transition && next.challengeWon == challengeWon;
        }
    };

    RenderDevice* device = nullptr;
    OnlineSession* session = nullptr;
    OnlineParty* selection = nullptr;
    std::unique_ptr<NativePlaySetup> setup;
    GameContext context;
    std::function<void(s32, s32, ControlFeedback)> vibrate;
    std::function<void(u16)> unlockClasses;
    std::unique_ptr<Level> level, parent;
    std::vector<PartyMember> levelMembers; ///< the party the current level was loaded with
    bool levelResumed = false;  ///< a parent brought back from a challenge cannot be reloaded
    f32 levelStopTimeTotal = 0; ///< the campaign's Stop Time total as the current level opened
    std::optional<LevelRef> current, secretParent;
    std::optional<Boundary> boundary;
    struct ConfirmedTerminal {
        NativeRollbackDriver::Status outcome;
        u64 tick;
    };
    std::optional<ConfirmedTerminal> terminal;
    std::optional<ShopSetup> shopSetup;
    PlayOptions options;
    OnlineLevelEntry entry;
    OnlineCompletion completion;
    AfterLevelScene shop;
    Phase phase = Phase::Closed;
    Failure failure = Failure::None;
    std::string failureDetail;
    NativeRollbackDriver::Status outcome = NativeRollbackDriver::Status::Held;
    u64 epoch = 0, entryEpoch = 0, completionEpoch = 0;
    u32 pausedDevices = 0;
    bool consoleCaptured = false;
    std::vector<std::string> consoleReplies;
    std::array<PlayScene::ConsoleCheats, InputCommand::kSeats> consoleCheats{};
    std::array<PlayScene::ConsoleCheats, InputCommand::kSeats> levelConsoleCheats{};
    bool advanced = false, afterShop = false, saveAfterShop = false, shopFinished = false;
    bool completedProgress = false;

    void quiet() {
        if (level) {
            level->scene.pauseGameplaySounds();
        }
        if (context.stopVibration) {
            context.stopVibration();
        }
        advanced = false;
    }
    void close() {
        quiet();
        entry.close();
        completion.close();
        shop.close();
        // Scene users borrow world, configuration and audio resources. Release
        // both native scenes before their campaign-owned configuration/timer.
        level.reset();
        parent.reset();
        levelMembers.clear();
        levelResumed = false;
        levelStopTimeTotal = 0;
        boundary.reset();
        terminal.reset();
        current.reset();
        secretParent.reset();
        shopSetup.reset();
        context = {};
        setup.reset();
        device = nullptr;
        session = nullptr;
        selection = nullptr;
        vibrate = {};
        unlockClasses = {};
        options = {};
        phase = Phase::Closed;
        failure = Failure::None;
        failureDetail.clear();
        outcome = NativeRollbackDriver::Status::Held;
        epoch = entryEpoch = completionEpoch = 0;
        pausedDevices = 0;
        consoleCaptured = false;
        consoleReplies.clear();
        consoleCheats = levelConsoleCheats = {};
        advanced = afterShop = saveAfterShop = shopFinished = false;
        completedProgress = false;
    }
    Phase fail(Failure reason) {
        if (phase == Phase::Failed) {
            return phase;
        }
        // Preserve the first cause before leaving releases the session's owners.
        // In particular, do not replace a desync/timeout with its teardown error.
        if (session != nullptr) {
            failureDetail = session->failureDetail();
            if (failureDetail.empty()) {
                // A local observer may fail before OnlineSession's next flush
                // promotes its match failure. Preserve that cause as well.
                failureDetail = session->match().failureDetail();
            }
        }
        if (failureDetail.empty()) {
            failureDetail = failureDescription(reason);
        }
        log::error("Online stage stopped at epoch {}: {}", epoch, failureDetail);
        quiet();
        entry.close();
        completion.close();
        shop.close();
        failure = reason;
        phase = Phase::Failed;
        if (session != nullptr) {
            session->leave();
        }
        return phase;
    }
    bool prepareBoundary() {
        if (!boundary || !session->host()) {
            return false;
        }
        const auto& next = *boundary;
        return session->match().prepare(next.scene, next.transition, &next.profiles,
                                        next.shop ? &*next.shop : nullptr, next.challengeWon);
    }
    bool travel() {
        if (!level || boundary || outcome != NativeRollbackDriver::Status::Travel) {
            fail(Failure::Scene);
            return false;
        }
        const auto& scene = level->scene;
        const auto& world = level->world;
        for (usize seat = 0; seat < consoleCheats.size(); ++seat) {
            consoleCheats[seat] = scene.cheats(static_cast<s32>(seat));
        }
        const auto& consoleTravel = scene.consoleTravelRequest();
        const bool returning = consoleTravel ? world.ref().isSecret() && secretParent &&
                                                   consoleTravel->destination == *secretParent
                                             : scene.secretTravel() && world.ref().isSecret();
        const bool entering = consoleTravel
                                  ? consoleTravel->destination.isSecret() && !world.ref().isSecret()
                                  : scene.secretTravel() && !returning;
        if (returning && (!parent || !secretParent)) {
            fail(Failure::Scene);
            return false;
        }
        const auto destination = returning ? *secretParent : scene.destination();
        const auto id = NativeOnlinePlay::sceneId(*context.levels, destination);
        if (!id) {
            fail(Failure::Scene);
            return false;
        }
        Boundary next;
        next.sourceEpoch = epoch;
        next.consoleTravel = consoleTravel.has_value();
        next.sourceTick = level->driver.confirmedTick();
        next.scene = *id;
        next.members = scene.party();
        const auto* info = world.level();
        if (!consoleTravel && !world.isTower() && !scene.secretTravel()) {
            recordLevelBeaten(next.members, world.ref().realmId, world.ref().index,
                              info != nullptr ? info->rune : 0, info != nullptr ? info->legend : 0);
        }
        const auto profiles = profilesOf(next.members, session->match().context().owners);
        if (!profiles) {
            fail(Failure::Party);
            return false;
        }
        next.profiles = *profiles;
        if (entering || returning) {
            next.transition =
                entering ? MatchTransition::SecretEnter : MatchTransition::SecretReturn;
            next.challengeWon = !consoleTravel && returning && scene.challenge().won();
        } else if (!consoleTravel && !world.isTower() && destination.isTower()) {
            const auto source = NativeOnlinePlay::sceneId(*context.levels, world.ref());
            if (!source) {
                fail(Failure::Scene);
                return false;
            }
            next.transition = MatchTransition::Shop;
            next.scene = *source;
            ShopSetup shopState;
            shopState.source = next.scene;
            shopState.destination = *id;
            shopState.expanded = context.config->game.expandedShop;
            shopState.ending = info != nullptr && (info->bossType == 43 || info->bossType == 44) &&
                               scene.victory().finished();
            shopState.maxima =
                info != nullptr ? info->shopMaxima : std::array<s32, 3>{1000, 100, 1000};
            for (const auto& result : scene.levelResults()) {
                if (result.player < 0 ||
                    result.player >= static_cast<s32>(shopState.totals.size())) {
                    fail(Failure::Party);
                    return false;
                }
                shopState.totals[static_cast<usize>(result.player)] = result.totals;
            }
            for (const auto& member : next.members) {
                if (member.fallen) {
                    shopState.fallen.insert(static_cast<usize>(member.player));
                }
            }
            if (!shopState.valid()) {
                fail(Failure::Scene);
                return false;
            }
            next.shop = shopState;
        }
        boundary = std::move(next);
        if (const auto* pending = session->match().pendingNativeContext();
            pending != nullptr && !boundary->matches(*pending)) {
            fail(Failure::Scene);
            return false;
        }
        if (!session->match().completeNativeBoundary(boundary->sourceEpoch, boundary->sourceTick)) {
            fail(Failure::Session);
            return false;
        }
        options = {};
        options.welcome = false;
        options.arriving = true;
        options.arrivalWorld = static_cast<u32>(std::max(world.ref().realmId, 0));
        if (consoleTravel) {
            options.arriving = false;
            options.position = consoleTravel->position;
            options.yaw = consoleTravel->yaw;
        }
        // The guest retains this complete native continuation even if its local
        // terminal tick precedes the host's Prepare. No profiles replace it.
        if (session->host() && !prepareBoundary()) {
            fail(Failure::Session);
            return false;
        }
        quiet();
        phase = Phase::Loading;
        return true;
    }

    bool checkpointLocals(const MatchParty& profiles, bool save) {
        if (!selection->checkpoint(*session, profiles)) {
            fail(Failure::Party);
            return false;
        }
        completedProgress = true;
        if (save && context.config->game.autoSave && context.saveLocalParty) {
            context.saveLocalParty(selection->localMembers());
        }
        // Local save slots never flow back into the canonical native party.
        return true;
    }

    bool loadShop() {
        auto& match = session->match();
        const auto* received = match.shop().setup();
        if (received == nullptr) {
            return false;
        }
        if (!boundary || !boundary->shop || !sameShop(*boundary->shop, *received)) {
            fail(Failure::Scene);
            return false;
        }
        const auto source = NativeOnlinePlay::levelOf(*context.levels, received->source);
        const auto destination = NativeOnlinePlay::levelOf(*context.levels, received->destination);
        if (!source || !destination || source->isTower() || source->isSecret() ||
            !destination->isTower() || secretParent) {
            fail(Failure::Scene);
            return false;
        }
        if (completionEpoch != match.context().epoch) {
            if (!level || level->world.ref() != *source || level->world.level() == nullptr) {
                fail(Failure::Scene);
                return false;
            }
            const s32 boss = level->world.level()->bossType;
            quiet();
            level.reset();
            entry.close();
            completion.open(*device, context, *source, boss, received->ending);
            completionEpoch = match.context().epoch;
        } else {
            completion.update(context);
        }
        if (completion.phase() == OnlineCompletion::Phase::Failed) {
            fail(Failure::Assets);
            return false;
        }
        if (completion.phase() != OnlineCompletion::Phase::Ready) {
            return false;
        }
        std::vector<LevelResults> results;
        for (const auto& member : boundary->members) {
            results.push_back({member.player, received->totals[static_cast<usize>(member.player)]});
        }
        const auto visit = completion.selection().visit;
        if (!shop.open(*device, context, boundary->members, results, received->maxima, source->name,
                       visit)) {
            fail(Failure::Assets);
            return false;
        }
        shopSetup = *received;
        epoch = match.context().epoch;
        afterShop = true;
        saveAfterShop = true;
        shopFinished = false;
        boundary.reset();
        if (!match.loaded()) {
            fail(Failure::Session);
            return false;
        }
        return true;
    }

    bool load(const SessionInputs::Frame& devices) {
        auto& match = session->match();
        const auto& next = match.context();
        const bool initial = epoch == 0;
        const MatchParty* profiles = initial ? session->party() : match.travelParty();
        if (profiles == nullptr) {
            return false;
        }
        if (initial) {
            const auto canonical = profilesOf(setup->party(), next.owners);
            if (next.transition != MatchTransition::Start || next.scene != setup->scene() ||
                !canonical || !sameProfiles(*canonical, *profiles)) {
                fail(Failure::Party);
                return false;
            }
        } else if (!boundary || !boundary->matches(next) ||
                   !sameProfiles(boundary->profiles, *profiles)) {
            fail(Failure::Party);
            return false;
        }
        if (next.transition == MatchTransition::Shop) {
            return loadShop();
        }
        const auto destination = NativeOnlinePlay::levelOf(*context.levels, next.scene);
        if (!destination || next.transition == MatchTransition::Resume) {
            fail(Failure::Scene);
            return false;
        }
        const bool entering = next.transition == MatchTransition::SecretEnter;
        const bool returning = next.transition == MatchTransition::SecretReturn;
        const auto members =
            initial ? setup->party() : std::span<const PartyMember>(boundary->members);
        if (entryEpoch != next.epoch) {
            if (entering) {
                if (!destination->isSecret() || !current || current->isSecret() || secretParent ||
                    parent || !level) {
                    fail(Failure::Scene);
                    return false;
                }
                secretParent = current;
                level->unbind();
                level->scene.suspendForChallenge();
                parent = std::move(level);
            } else if (returning) {
                if (!current || !current->isSecret() || !secretParent ||
                    *secretParent != *destination || !parent) {
                    fail(Failure::Scene);
                    return false;
                }
                if (!checkpointLocals(*profiles, false)) {
                    return false;
                }
                const s32 character = SecretChallenge::classFor(current->index);
                if (next.challengeWon && character >= 8 && character < kClassCount) {
                    const auto reward = static_cast<u16>(1U << static_cast<u32>(character - 8));
                    for (auto& member : selection->localMembers()) {
                        member.save.classUnlock |= reward;
                    }
                    if (unlockClasses) {
                        unlockClasses(reward);
                    }
                    if (context.config->game.autoSave && context.saveLocalParty) {
                        context.saveLocalParty(selection->localMembers());
                    }
                }
            } else if (boundary && boundary->consoleTravel) {
                // Debug travel is still a confirmed common boundary, but it does not
                // award a level or force the normal results/shop route. Replacing a
                // challenge keeps its suspended parent; leaving it discards that parent.
                if (!destination->isSecret()) {
                    parent.reset();
                    secretParent.reset();
                }
            } else if (destination->isSecret() || secretParent) {
                fail(Failure::Scene);
                return false;
            }
            if (afterShop) {
                if (!checkpointLocals(*profiles, saveAfterShop)) {
                    return false;
                }
                afterShop = saveAfterShop = false;
                shop.close();
                shopSetup.reset();
            }
            level.reset();
            completion.close();
            entry.open(*device, context, *destination, members, returning);
            entryEpoch = next.epoch;
        } else {
            const auto local = selection->inputs(*session, devices);
            if (!local) {
                fail(Failure::Input);
                return false;
            }
            if (entry.phase() == OnlineLevelEntry::Phase::Movie) {
                for (usize index = 0; index < session->seats().size(); ++index) {
                    if ((*local)[index].movieSkipPressed &&
                        !match.requestMovieSkip(session->seats()[index])) {
                        fail(Failure::Input);
                        return false;
                    }
                }
            }
            entry.update(*device, context, match.movieSkipReady());
        }
        if (entry.phase() == OnlineLevelEntry::Phase::Failed) {
            fail(Failure::Assets);
            return false;
        }
        if (entry.phase() != OnlineLevelEntry::Phase::Ready) {
            return false;
        }
        levelMembers.assign(members.begin(), members.end());
        levelResumed = returning;
        levelConsoleCheats = consoleCheats;
        levelStopTimeTotal = setup->stopTimeTotal();
        if (returning) {
            level = std::move(parent);
            level->scene.resumeFromChallenge(members);
            for (usize seat = 0; seat < consoleCheats.size(); ++seat) {
                level->scene.setCheats(static_cast<s32>(seat), consoleCheats[seat]);
            }
            if (boundary && boundary->consoleTravel && options.position) {
                level->scene.relocate(*options.position);
            }
            if (!level->bind(match) || !applyDrops(*level)) {
                fail(Failure::Scene);
                return false;
            }
        } else {
            auto candidate = std::make_unique<Level>();
            if (!candidate->world.load(*device, context.unpackedRoot, *destination) ||
                !candidate->scene.open(*device, context, candidate->world, members, options)) {
                fail(Failure::Assets);
                return false;
            }
            candidate->scene.setLocalSeats(seatMask(session->seats()));
            candidate->scene.setSoloCameras(context.config != nullptr &&
                                            context.config->multiplayer.soloCamera);
            candidate->scene.setFixedRoster(true);
            candidate->scene.setViewSeat(lowestSeat(session->seats()));
            for (usize seat = 0; seat < consoleCheats.size(); ++seat) {
                candidate->scene.setCheats(static_cast<s32>(seat), consoleCheats[seat]);
            }
            if (!candidate->scene.nativePresentationReady() || !candidate->bind(match)) {
                fail(Failure::Assets);
                return false;
            }
            if (!applyDrops(*candidate)) {
                fail(Failure::Scene);
                return false;
            }
            level = std::move(candidate);
        }
        epoch = next.epoch;
        current = *destination;
        boundary.reset();
        outcome = NativeRollbackDriver::Status::Held;
        terminal.reset();
        if (returning) {
            secretParent.reset();
        }
        if (!match.loaded()) {
            fail(Failure::Session);
            return false;
        }
        log::info("Native online stage loaded: {} (epoch {}, peer {})", destination->name, epoch,
                  match.localPeer());
        return true;
    }

    /** Every drop so far holds its seats still in the level's driver. */
    bool applyDrops(Level& target) {
        for (const auto& drop : session->match().dropsFor(session->match().context().epoch)) {
            if (!target.driver.suspend(drop.peer, drop.tick)) {
                return false;
            }
        }
        return true;
    }
    /** The actual input the level's driver holds past `through`, which a rebuild keeps: this
     * peer's own was sent and the room's acknowledged, so the room sends neither again. A
     * dropped peer's seats hold still from its drop by rule instead. */
    std::vector<InputCommand> heldInputs(u64 through) const {
        const auto& match = session->match();
        const auto drops = match.dropsFor(match.context().epoch);
        auto held = level->driver.actualInputsAfter(through);
        std::erase_if(held, [&](const InputCommand& command) {
            return std::ranges::any_of(drops, [&](const MatchSession::Drop& drop) {
                return match.context().owners[command.seat] == drop.peer &&
                       command.tick >= drop.tick;
            });
        });
        return held;
    }
    /** The host's: the confirmed journal to the resync tick plus the unconfirmed tail it
     * holds, queued for every peer that owes a rebuild. */
    bool serveJournal(MatchSession& match, const MatchSession::Resync& pending) {
        if (!level || level->driver.journal().through() < pending.tick) {
            return false;
        }
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (!pending.peers.contains(peer)) {
                continue;
            }
            auto chunks = JournalPacket::chunks(level->driver.journal(), 1, pending.tick, false);
            auto tail = JournalPacket::tailChunks(
                epoch, level->driver.actualInputsAfter(pending.tick), true);
            if (chunks.empty()) {
                return false;
            }
            if (tail.empty()) {
                chunks.back().last = true;
            }
            chunks.insert(chunks.end(), std::make_move_iterator(tail.begin()),
                          std::make_move_iterator(tail.end()));
            for (const auto& chunk : chunks) {
                if (!match.queueJournal(peer, chunk)) {
                    return false;
                }
            }
            if (!match.journalQueued(peer)) {
                return false;
            }
            log::info("Native online resync: {} journal chunks to tick {} queued for peer {}",
                      chunks.size(), pending.tick, peer);
        }
        return true;
    }
    /** A named peer's: reload the stage as the epoch began and replay the journal to the
     * resync tick, then rejoin the room's input where its own recorded input ends. A parent
     * resumed from a challenge began its epoch where no load can bring it back, so its own
     * scene returns to the state the epoch was bound in instead. */
    Phase rebuild(MatchSession& match) {
        const auto through = match.resyncRequested();
        if (!through || !level || !current) {
            return fail(Failure::Scene);
        }
        const auto commands = match.takeJournal();
        const auto started = std::chrono::steady_clock::now();
        const auto held = heldInputs(*through);
        // Held input lies past the resync tick, where only the host's tail can hold it too.
        std::set<std::pair<u64, u8>> tail;
        for (const auto& recorded : commands) {
            if (recorded.tick > *through) {
                tail.emplace(recorded.tick, recorded.seat);
            }
        }
        const auto absent = std::ranges::count_if(
            held, [&](const InputCommand& kept) { return !tail.contains({kept.tick, kept.seat}); });
        log::warn("Native online resync: rebuilding {} to tick {} from {} journal commands",
                  current->name, *through, commands.size());
        log::info("Native resync state: epoch={} peer={} prior_next={} prior_confirmed={} "
                  "prior_sample={} held={} held_absent_from_host={}",
                  epoch, match.localPeer(), level->driver.nextTick(), level->driver.confirmedTick(),
                  level->driver.sampleTick(), held.size(), absent);
        // The binding is restorable only while the scene still records the epoch's sound.
        if (levelResumed && !level->driver.restoreBinding()) {
            log::error("Native online resync: the resumed stage could not return to its start");
            return fail(Failure::Scene);
        }
        level->unbind();
        // A resumed parent goes on in its own scene; any other stage is loaded afresh.
        std::unique_ptr<Level> candidate;
        if (levelResumed) {
            candidate = std::move(level);
        } else {
            // The abandoned timeline may have moved the campaign's Stop Time total (a pickup
            // predicted or diverged); the stage opens again with the one its epoch opened with.
            setup->stopTimeTotal() = levelStopTimeTotal;
            candidate = std::make_unique<Level>();
            if (!candidate->world.load(*device, context.unpackedRoot, *current) ||
                !candidate->scene.open(*device, context, candidate->world, levelMembers, options)) {
                return fail(Failure::Assets);
            }
            candidate->scene.setLocalSeats(seatMask(session->seats()));
            candidate->scene.setSoloCameras(context.config != nullptr &&
                                            context.config->multiplayer.soloCamera);
            candidate->scene.setFixedRoster(true);
            candidate->scene.setViewSeat(lowestSeat(session->seats()));
            for (usize seat = 0; seat < levelConsoleCheats.size(); ++seat) {
                candidate->scene.setCheats(static_cast<s32>(seat), levelConsoleCheats[seat]);
            }
        }
        if (!candidate->scene.nativePresentationReady() ||
            !(levelResumed ? candidate->rebindDriver(match) : candidate->bindDriver(match)) ||
            !applyDrops(*candidate) || !candidate->driver.adopt(commands) ||
            (!held.empty() && !candidate->driver.adopt(held)) ||
            !candidate->driver.beginRebuild(*through)) {
            log::error(
                "Native online resync: the stage could not be reloaded or the journal adopted");
            return fail(Failure::Scene);
        }
        while (candidate->driver.nextTick() <= *through) {
            // Outputs of replayed ticks were heard and felt before the divergence.
            const auto status = candidate->driver.advance(true).status;
            if (status != NativeRollbackDriver::Status::Advanced) {
                log::error(
                    "Native online resync: replay stopped at tick {} (status {}, failure {})",
                    candidate->driver.nextTick(), static_cast<u32>(status),
                    static_cast<u32>(candidate->driver.failure()));
                return fail(Failure::Scene);
            }
        }
        if (!candidate->driver.finishRebuild()) {
            log::error(
                "Native online resync: the rebuilt driver could not resume sampling at tick {}",
                candidate->driver.nextTick());
            return fail(Failure::Scene);
        }
        if (!candidate->attach(match)) {
            log::error("Native online resync: the rebuilt scene could not attach to the room");
            return fail(Failure::Scene);
        }
        level = std::move(candidate);
        terminal.reset();
        outcome = NativeRollbackDriver::Status::Held;
        if (!match.finishResync()) {
            return fail(Failure::Session);
        }
        log::info("Native online resync: {} rebuilt to tick {} (epoch {}, peer {})", current->name,
                  *through, epoch, match.localPeer());
        log::info("Native resync resumed: next={} confirmed={} sample={} rebuild_ms={:.1f}",
                  level->driver.nextTick(), level->driver.confirmedTick(),
                  level->driver.sampleTick(),
                  std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started)
                      .count());
        return phase = Phase::Playing;
    }
    Phase updateShop(const SessionInputs::Frame& devices) {
        completion.close();
        auto& match = session->match();
        auto& sync = match.shop();
        if (!shopFinished) {
            const auto local = selection->inputs(*session, devices);
            if (!local) {
                return fail(Failure::Input);
            }
            for (usize index = 0; index < session->seats().size(); ++index) {
                const auto seat = session->seats()[index];
                auto input = (*local)[index].menu;
                const auto lane =
                    std::ranges::find(shop.session().lanes(), static_cast<s32>(seat),
                                      [](const ShopLane& value) { return value.member.player; });
                if (lane == shop.session().lanes().end()) {
                    return fail(Failure::Party);
                }
                if (lane->phase == ShopPhase::Shopping) {
                    const auto localDevice = selection->device(*session, seat);
                    if (!localDevice) {
                        return fail(Failure::Input);
                    }
                    input = AfterLevelScene::captureShopInput(input, *context.config, *localDevice);
                }
                const auto focus = shop.captureOnlinePointer(static_cast<s32>(seat), input);
                if (!sync.sample(seat, input, focus ? static_cast<s32>(*focus) : -1,
                                 static_cast<u8>(lane->phase), lane->cursor)) {
                    return fail(Failure::Input);
                }
            }
        }
        phase = Phase::Shopping;
        // The host steps the shop once a tick; a guest runs every step it holds, so a stall
        // or a burst of delivery leaves it no further behind than one poll brings.
        for (auto step = match.host() ? sync.advance() : sync.take(); step;
             step = match.host() ? std::nullopt : sync.take()) {
            // A dropped player's lane ends rather than holding the shop, at the step the host
            // ended it at on every peer, however late the drop reached it.
            for (usize seat = 0; seat < match.context().owners.size(); ++seat) {
                if (match.shopLaneGone(match.context().owners[seat], epoch, step->tick)) {
                    shop.abandonLane(static_cast<s32>(seat));
                }
            }
            for (usize seat = 0; seat < step->focus.size(); ++seat) {
                if (match.host() && step->observedPhase[seat] != 255) {
                    const auto lane = std::ranges::find(
                        shop.session().lanes(), static_cast<s32>(seat),
                        [](const ShopLane& value) { return value.member.player; });
                    if (lane == shop.session().lanes().end() ||
                        step->observedPhase[seat] != static_cast<u8>(lane->phase)) {
                        step->inputs[seat] = {};
                        step->focus[seat] = -1;
                    } else if (step->focus[seat] == -1 &&
                               step->observedCursor[seat] != lane->cursor) {
                        step->inputs[seat].select = step->inputs[seat].back = false;
                    }
                }
                if (step->focus[seat] >= 0 &&
                    !shop.focusOnline(static_cast<s32>(seat),
                                      static_cast<usize>(step->focus[seat]))) {
                    if (!match.host()) {
                        return fail(Failure::Presentation);
                    }
                    step->focus[seat] = -1;
                    step->inputs[seat].select = step->inputs[seat].back = false;
                }
            }
            shop.setCuesHeard(NativeOnlinePlay::shopCuesHeard(match.host(), sync.held()));
            const bool finished =
                shop.update(1.0 / 60, step->inputs, &step->potions,
                            match.host() ? &step->inputs : nullptr, !match.host());
            if (match.host() && !sync.publish(*step)) {
                return fail(Failure::Session);
            }
            if (finished && !shopFinished) {
                if (!shopSetup) {
                    return fail(Failure::Scene);
                }
                Boundary next;
                next.sourceEpoch = epoch;
                next.scene = shopSetup->destination;
                next.members = shop.session().party();
                for (auto& member : next.members) {
                    member.resultsCheckpoint.reset();
                }
                const auto profiles = profilesOf(next.members, match.context().owners);
                if (!profiles) {
                    return fail(Failure::Party);
                }
                next.profiles = *profiles;
                boundary = std::move(next);
                shopFinished = true;
                if (!sync.finish()) {
                    return fail(Failure::Session);
                }
            }
        }
        if (match.host() && sync.finished()) {
            if (!prepareBoundary()) {
                return fail(Failure::Session);
            }
            phase = Phase::Loading;
        }
        return phase;
    }

    Phase updateFrame(const SessionInputs::Frame& devices) {
        if (session == nullptr || phase == Phase::Failed) {
            return phase;
        }
        advanced = false;
        auto localDevices = devices;
        for (usize index = 0; index < localDevices.size(); ++index) {
            if (consoleCaptured || (pausedDevices & (1U << index)) != 0) {
                localDevices[index] = {};
            }
        }
        session->update(1.0 / 60);
        auto& match = session->match();
        if (session->phase() != OnlineSession::Phase::Active ||
            match.phase() == MatchSession::Phase::Stopped || !match.nativeInputs()) {
            return fail(Failure::Session);
        }
        if (phase == Phase::Finished) {
            // Keep relaying the source inputs. Another peer may still need to
            // execute this terminal frame; application departure is explicit.
            return phase;
        }
        if (boundary && match.pendingNativeContext()) {
            if (!boundary->matches(*match.pendingNativeContext()) ||
                !match.completeNativeBoundary(boundary->sourceEpoch, boundary->sourceTick)) {
                return fail(Failure::Scene);
            }
        }
        if (match.phase() == MatchSession::Phase::Loading && match.context().epoch != epoch) {
            phase = Phase::Loading;
            if (!load(localDevices)) {
                return phase;
            }
        }
        if (match.phase() != MatchSession::Phase::Running) {
            if (level && level->scene.recordedAudioEpoch() != 0) {
                level->scene.serviceRecordedAudio(1.0f / 60);
            }
            return phase;
        }
        if (epoch == 0 || epoch != match.context().epoch) {
            return fail(Failure::Scene);
        }
        if (match.context().transition == MatchTransition::Shop) {
            return updateShop(localDevices);
        }
        if (!level) {
            return fail(Failure::Scene);
        }
        if (boundary && match.pendingNativeContext() &&
            !boundary->matches(*match.pendingNativeContext())) {
            return fail(Failure::Scene);
        }
        for (const auto& drop : match.takeDrops()) {
            if (drop.epoch == epoch && !level->driver.suspend(drop.peer, drop.tick)) {
                return fail(Failure::Input);
            }
        }
        if (const auto pending = match.pendingResync()) {
            if (!serveJournal(match, *pending)) {
                return fail(Failure::Session);
            }
        }
        if (match.resyncRequested()) {
            if (!match.journalReady()) {
                // Hold the diverged scene still; the room's input keeps arriving.
                quiet();
                return phase = Phase::Resyncing;
            }
            return rebuild(match);
        }
        const auto local = selection->inputs(*session, localDevices);
        auto console =
            match.host() && level->driver.sampleReady() && !level->driver.tentativeTerminal()
                ? match.takeConsoleRequest()
                : std::nullopt;
        if (!local || !level->driver.sample(*local, std::move(console))) {
            return fail(Failure::Input);
        }
        if (!match.publishNativeInputs(level->driver.redundantInputs(), level->driver.nextTick()) ||
            !session->flush()) {
            return fail(Failure::Session);
        }
        // Another guest's input that a direct link brought sooner confirms only as far as the
        // host holds it: the host drops a guest from the tick after its own receipt.
        if (!match.reportHostReceipts(level->driver)) {
            return fail(Failure::Input);
        }
        const auto end = match.nativeEndTick();
        const auto result = level->driver.advance(!end || level->driver.nextTick() <= *end);
        for (const auto& reply : result.consoleReplies) {
            if (selection->device(*session, reply.seat)) {
                if (consoleReplies.size() >= 64) {
                    consoleReplies.erase(consoleReplies.begin());
                }
                consoleReplies.push_back(reply.text);
            }
        }
        if (result.status == NativeRollbackDriver::Status::Failed) {
            log::error("Native rollback failed at epoch {} tick {}: {}", epoch,
                       level->driver.nextTick(), static_cast<u32>(level->driver.failure()));
            return fail(Failure::Scene);
        }
        if (!match.publishNativeInputs(level->driver.redundantInputs(), level->driver.nextTick()) ||
            !match.confirmNativeTick(epoch, level->driver.confirmedTick())) {
            return fail(Failure::Session);
        }
        // These are retained observations from their corrected tick, not a hash
        // of today's speculative head mislabeled as an older confirmed frame.
        for (const auto& observation : result.digests) {
            if (!match.observeNative(observation.tick, observation.digest)) {
                log::error("Native simulation agreement failed at epoch {} tick {}", epoch,
                           observation.tick);
                return fail(Failure::Scene);
            }
        }
        const bool terminalResult = result.status == NativeRollbackDriver::Status::Travel ||
                                    result.status == NativeRollbackDriver::Status::GameOver ||
                                    result.status == NativeRollbackDriver::Status::Leave;
        if (terminalResult) {
            if (terminal || !result.terminalDigest ||
                result.terminalDigest->tick != level->driver.confirmedTick() ||
                !match.observeNativeTerminal(epoch, result.terminalDigest->tick,
                                             result.terminalDigest->digest)) {
                return fail(Failure::Scene);
            }
            terminal = ConfirmedTerminal{result.status, result.terminalDigest->tick};
        } else if (result.terminalDigest) {
            return fail(Failure::Scene);
        }
        if (!result.audio) {
            return fail(Failure::Scene);
        }
        for (const auto& action : result.actions) {
            if (!level->scene.dispatchOutput(action)) {
                return fail(Failure::Scene);
            }
        }
        level->scene.dispatchRecordedAudio(*result.audio);
        level->scene.serviceRecordedAudio(1.0f / 60);
        if (end && level->driver.confirmedTick() >= *end &&
            (!terminal || terminal->outcome != NativeRollbackDriver::Status::Travel) && !boundary) {
            return fail(Failure::Scene);
        }
        if (terminal && match.nativeTerminalAgreed(epoch, terminal->tick)) {
            // Keep the source scene/input journal alive while every peer checks
            // the exact terminal state, including boundaries between periodic
            // hashes. Receipt of input alone cannot release a campaign barrier.
            outcome = terminal->outcome;
            terminal.reset();
            if (outcome == NativeRollbackDriver::Status::Travel) {
                travel();
            } else {
                quiet();
                phase = Phase::Finished;
            }
        } else if (!boundary) {
            outcome = terminal ? NativeRollbackDriver::Status::Held : result.status;
            advanced = !terminal && result.status == NativeRollbackDriver::Status::Advanced;
            phase = Phase::Playing;
            entry.close();
        }
        // Confirmed travel may have adopted a queued source-fenced epoch.
        // The old scene and terminal party remain intact until load validates it.
        if (phase != Phase::Failed && match.context().epoch != epoch) {
            if (!boundary || !boundary->matches(match.context())) {
                return fail(Failure::Scene);
            }
            phase = Phase::Loading;
        }
        return phase;
    }
};

NativeOnlinePlay::NativeOnlinePlay() : m_data(std::make_unique<Data>()) {}
NativeOnlinePlay::~NativeOnlinePlay() {
    close();
}

std::optional<u32> NativeOnlinePlay::sceneId(const LevelCatalog& catalog, const LevelRef& level) {
    if (level.isTower()) {
        return level.name == "L1" && (level == LevelRef::tower() || catalog.byName("L1") == level)
                   ? std::optional<u32>{14 * 256 + 1}
                   : std::nullopt;
    }
    if (level.realmId < 0 || level.realmId >= 255 || level.index < 0 || level.index >= 255 ||
        catalog.byName(level.name) != level) {
        return std::nullopt;
    }
    return static_cast<u32>((level.realmId + 1) * 256 + level.index + 1);
}
std::optional<LevelRef> NativeOnlinePlay::levelOf(const LevelCatalog& catalog, u32 scene) {
    if (scene == 14 * 256 + 1) {
        return LevelRef::tower();
    }
    if (scene == 0 || scene > 65535 || scene % 256 == 0) {
        return std::nullopt;
    }
    const auto realmId = static_cast<s32>(scene / 256) - 1;
    const auto index = static_cast<usize>(scene % 256 - 1);
    for (const auto& realm : catalog.realms()) {
        if (realm.id == realmId && realm.id != LevelRef::kTowerRealm &&
            index < realm.levels.size()) {
            const auto level = catalog.byName(realm.levels[index]);
            if (level && sceneId(catalog, *level) == scene) {
                return level;
            }
        }
    }
    return std::nullopt;
}
bool NativeOnlinePlay::open(RenderDevice& device, const GameContext& context,
                            OnlineSession& session, OnlineParty& selection,
                            std::unique_ptr<NativePlaySetup> setup) {
    auto& data = *m_data;
    const auto& match = session.match();
    const bool starting = !session.host() && match.phase() == MatchSession::Phase::Loading &&
                          match.context().transition == MatchTransition::Start;
    if (data.phase != Phase::Closed || !setup || !match.nativeInputs() ||
        setup->digest() != session.confirmedNativeSetup() ||
        session.phase() != OnlineSession::Phase::Active ||
        (match.phase() != MatchSession::Phase::Lobby && !starting) || context.levels == nullptr ||
        !context.levels->loaded() || !levelOf(*context.levels, setup->scene()) ||
        session.party() == nullptr) {
        return false;
    }
    const auto profiles = profilesOf(setup->party(), match.context().owners);
    // Lobby context may not carry its roster until the host prepares. The
    // bootstrap party is already complete, so validate it directly below too.
    MatchOwners owners{};
    for (usize seat = 0; seat < owners.size(); ++seat) {
        owners[seat] = (*session.party())[seat] ? 1 : 0;
    }
    const auto bootstrap = profilesOf(setup->party(), owners);
    if (!bootstrap || !sameProfiles(*bootstrap, *session.party()) ||
        (starting && (!profiles || match.context().scene != setup->scene())) ||
        !selection.inputs(session, {})) {
        return false;
    }
    if (session.host() && !session.match().prepare(setup->scene())) {
        return false;
    }
    data.device = &device;
    data.session = &session;
    data.selection = &selection;
    data.options = setup->options();
    data.setup = std::move(setup);
    data.context = context;
    data.context.config = &data.setup->config();
    data.context.stopTimeTotal = &data.setup->stopTimeTotal();
    data.context.tower = nullptr;
    data.context.saveSettings = {};
    data.context.unlockClasses = {};
    data.vibrate = context.vibrate;
    data.unlockClasses = context.unlockClasses;
    data.context.vibrate = [&data](s32 seat, s32 frames, ControlFeedback cue) {
        if (seat < 0 || seat >= static_cast<s32>(InputCommand::kSeats) || !data.vibrate) {
            return;
        }
        const auto local = data.selection->device(*data.session, static_cast<u8>(seat));
        if (local && (data.pausedDevices & (1U << static_cast<u32>(*local))) == 0) {
            data.vibrate(*local, frames, cue);
        }
    };
    data.phase = Phase::Loading;
    return true;
}
void NativeOnlinePlay::close() {
    m_data->close();
}
NativeOnlinePlay::Phase NativeOnlinePlay::update(const SessionInputs::Frame& devices) {
    auto& data = *m_data;
    try {
        const auto result = data.updateFrame(devices);
        if (data.session != nullptr && result != Phase::Failed && !data.session->flush()) {
            return data.fail(Failure::Session);
        }
        return phase();
    } catch (const std::exception& error) {
        log::error("Native online stage failed: {}", error.what());
        return data.fail(Failure::Scene);
    }
}
bool NativeOnlinePlay::pause(s32 device) {
    auto& data = *m_data;
    if (data.session == nullptr ||
        (data.phase != Phase::Playing && data.phase != Phase::Loading &&
         data.phase != Phase::Shopping) ||
        device < -1 || device >= static_cast<s32>(InputCommand::kSeats)) {
        return false;
    }
    u32 mask = 0;
    for (const auto seat : data.session->seats()) {
        const auto local = data.selection->device(*data.session, seat);
        if (local && (device == -1 || *local == device)) {
            // The overlay also consumes taps collected while prediction was
            // blocked. Already published frames remain immutable; another
            // local device keeps its own pending controls.
            if (data.level && data.level->boundEpoch != 0 &&
                !data.level->driver.clearPendingInput(seat)) {
                data.fail(Failure::Scene);
                return false;
            }
            mask |= 1U << static_cast<u32>(*local);
        }
    }
    data.pausedDevices |= mask;
    if (mask != 0 && data.context.stopVibration) {
        data.context.stopVibration();
    }
    return mask != 0;
}
bool NativeOnlinePlay::resume(s32 device) {
    if (device < -1 || device >= static_cast<s32>(InputCommand::kSeats)) {
        return false;
    }
    const u32 mask = device == -1 ? m_data->pausedDevices : (1U << static_cast<u32>(device));
    const bool captured = (m_data->pausedDevices & mask) != 0;
    m_data->pausedDevices &= ~mask;
    return captured;
}
bool NativeOnlinePlay::hasCompletedProgress() const {
    return m_data->completedProgress && m_data->selection != nullptr &&
           static_cast<bool>(m_data->context.manualSaveLocalParty);
}
bool NativeOnlinePlay::saveCompletedProgress() {
    if (!hasCompletedProgress()) {
        return false;
    }
    try {
        // This is the authenticated completed local copy, not scene.party().
        // Saving may allocate local slots, which never feed the shared roster.
        return m_data->context.manualSaveLocalParty(m_data->selection->localMembers());
    } catch (const std::exception& error) {
        log::warn("Completed local progress could not be saved: {}", error.what());
        return false;
    }
}
void NativeOnlinePlay::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height,
                              f64 /*seconds*/, f32 frameBlend) {
    auto& data = *m_data;
    if (data.phase == Phase::Closed || data.phase == Phase::Failed) {
        return;
    }
    if (data.phase == Phase::Loading && data.completion.visible()) {
        data.completion.render(device, projection, width, height);
    } else if (data.shop.isOpen()) {
        data.shop.render(device, projection, width, height);
    } else if (data.phase == Phase::Loading && data.entry.visible()) {
        data.entry.render(device, projection, width, height);
    } else if (data.level) {
        data.level->scene.render(device, projection, width, height, false,
                                 data.advanced ? std::clamp(frameBlend, 0.0f, 1.0f) : 1.0f);
    }
}
NativeOnlinePlay::Phase NativeOnlinePlay::phase() const {
    return m_data->phase == Phase::Playing && m_data->pausedDevices != 0 ? Phase::Paused
                                                                         : m_data->phase;
}
NativeOnlinePlay::Failure NativeOnlinePlay::failure() const {
    return m_data->failure;
}
std::string_view NativeOnlinePlay::failureDetail() const {
    return m_data->failureDetail;
}
NativeRollbackDriver::Status NativeOnlinePlay::outcome() const {
    return m_data->outcome;
}
const PlayScene* NativeOnlinePlay::scene() const {
    return m_data->level ? &m_data->level->scene : nullptr;
}
const LevelWorld* NativeOnlinePlay::world() const {
    return m_data->level ? &m_data->level->world : nullptr;
}
void NativeOnlinePlay::consoleCapture(bool captured) {
    auto& data = *m_data;
    if (captured && !data.consoleCaptured && data.level && data.level->boundEpoch != 0) {
        for (const auto seat : data.session->seats()) {
            if (!data.level->driver.clearPendingInput(seat)) {
                data.fail(Failure::Input);
                return;
            }
        }
    }
    data.consoleCaptured = captured;
}
void NativeOnlinePlay::consoleView(bool hideHud, const std::optional<WorldCamera>& camera) {
    if (m_data->level) {
        m_data->level->scene.setHudHidden(hideHud);
        m_data->level->scene.setViewOverride(camera);
    }
}
void NativeOnlinePlay::consoleSelect(const std::optional<SceneSelection>& selection) {
    if (m_data->level) {
        m_data->level->scene.select(selection);
    }
}
std::optional<SceneSelection> NativeOnlinePlay::consolePick(Vec2 cursor) {
    return m_data->level ? m_data->level->scene.pick(cursor) : std::nullopt;
}
std::vector<std::string> NativeOnlinePlay::takeConsoleReplies() {
    return std::exchange(m_data->consoleReplies, {});
}
u64 NativeOnlinePlay::simulatedTick() const {
    const auto next = m_data->level ? m_data->level->driver.nextTick() : 0;
    return next > 0 ? next - 1 : 0;
}
u64 NativeOnlinePlay::confirmedTick() const {
    return m_data->level ? m_data->level->driver.confirmedTick() : 0;
}
NativeRollbackDriver::Statistics NativeOnlinePlay::rollbackStatistics() const {
    return m_data->level ? m_data->level->driver.statistics() : NativeRollbackDriver::Statistics{};
}
OnlineLevelEntry::Phase NativeOnlinePlay::entryPhase() const {
    return m_data->entry.phase();
}
bool NativeOnlinePlay::entryVisible() const {
    return m_data->entry.visible() || m_data->completion.visible();
}
const OnlineCompletion& NativeOnlinePlay::completion() const {
    return m_data->completion;
}
const AfterLevelScene* NativeOnlinePlay::shopScene() const {
    return m_data->shop.isOpen() ? &m_data->shop : nullptr;
}
} // namespace gdl::game
