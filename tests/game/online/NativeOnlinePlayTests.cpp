#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"

#include "FakeGameServices.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/ControlProfiles.h"
#include "game/level/LevelCatalog.h"
#include "game/level/TowerAccess.h"
#include "game/netplay/OnlinePair.h"
#include "game/netplay/OnlineParty.h"
#include "game/online/AgreedSetup.h"
#include "game/online/NativeOnlinePlay.h"
#include "game/players/PlayerActor.h"
#include "game/scenes/AfterLevelScene.h"
#include "game/scenes/NativeDigestProfileSupport.h"
#include "game/scenes/PlayScene.h"
#include "game/tower/LevelBeaten.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr std::array<u8, 2> kSeats{1, 3};
constexpr s32 kHostDevice = 2;
constexpr s32 kGuestDevice = 0;
constexpr s32 kSecondHostDevice = 3;
constexpr u8 kSecondHostSeat = 2;
constexpr s32 kRumbleFrames = 10; ///< how long each presented damage rumble lasts

/** A controller's rumble as a machine was asked for it. */
std::string rumble(s32 device, s32 frames, ControlFeedback cue) {
    return std::format("device {} frames {} cue {}", device, frames, static_cast<u32>(cue));
}

/** Every rumble a machine's devices were asked for, in turn. */
std::vector<std::string> rumbles(const test::RecordingHaptics& haptics) {
    std::vector<std::string> asked;
    asked.reserve(haptics.pulses.size());
    for (const auto& pulse : haptics.pulses) {
        asked.push_back(rumble(pulse.player, pulse.frames, pulse.feedback));
    }
    return asked;
}

/** A seat's damage rumble as a scene's output ledger presents it for a tick. */
PlayScene::OutputLedger::Action presented(u8 seat) {
    using Ledger = PlayScene::OutputLedger;
    return {
        .kind = Ledger::ActionKind::Present,
        .event = {.id = {.epoch = 1,
                         .tick = 1,
                         .emitter = {.nameSpace = 1, .entity = seat, .generation = 1, .seat = seat},
                         .cue = static_cast<u32>(ControlFeedback::Damage) + 1,
                         .occurrence = 0},
                  .policy = Ledger::Policy::OneShot,
                  .payload = PlayScene::RumbleOutput{.player = seat,
                                                     .frames = kRumbleFrames,
                                                     .feedback = ControlFeedback::Damage},
                  .loop = std::nullopt}};
}

struct NativePair {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    RoomSnapshot room{"ABCDEFGH",
                      std::string(32, 'a'),
                      1,
                      false,
                      {{std::string(32, 'a'), {1}, false}, {std::string(32, 'b'), {3}, false}}};
    test::OnlinePair::Service hostService{room, std::string(32, 'a')};
    test::OnlinePair::Service guestService{room, std::string(32, 'b')};
    test::OnlinePair::Wire hostWire, guestWire;
    OnlineSession hostSession{hostService, hostWire, true, 1};
    OnlineSession guestSession{guestService, guestWire, false, 1};
    test::FakeRenderDevice hostDevice, guestDevice;
    AudioMixer hostMixer{48000}, guestMixer{48000};
    SoundPlayer hostSounds{hostMixer}, guestSounds{guestMixer};
    GameConfig hostConfig, guestConfig;
    StringTable strings;
    LevelCatalog catalog;
    GameServices hostContext, guestContext;
    test::RecordingHaptics hostHaptics, guestHaptics; ///< what each machine's devices were asked
    test::RecordingUnlocks hostUnlocks, guestUnlocks;
    test::RecordingPartyStore hostStore, guestStore;
    OnlineParty hostParty, guestParty;
    // Only the diagnostic test opts into these catalogs. NativeOnlinePlay does
    // not require or publish old combat snapshots to run either native scene.
    NativeOnlinePlay host, guest;
    std::unique_ptr<NativePlaySetup> hostSetup, guestSetup;

    explicit NativePair(const PlayOptions& options = {}, std::string_view beatenLevel = {},
                        std::optional<bool> autoSave = {}, std::string_view initialLevel = "L1",
                        bool withAudio = false, u8 inputDelay = 1, bool secondHostPlayer = false)
        : hostSession(hostService, hostWire, true, static_cast<u8>(secondHostPlayer ? 2 : 1)) {
        if (secondHostPlayer) {
            room.members[0].seats.push_back(kSecondHostSeat);
        }
        hostWire.remote = &guestWire;
        guestWire.remote = &hostWire;
        hostDevice.recordDraws = guestDevice.recordDraws = false;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        hostConfig.camera.horizontalFovDegrees = 42;
        guestConfig.camera.horizontalFovDegrees = 97;
        hostConfig.timing.gameplayFrameRate = 30;
        guestConfig.timing.gameplayFrameRate = 120;
        hostConfig.game.autoSave = guestConfig.game.autoSave =
            autoSave.value_or(!beatenLevel.empty());
        hostContext.config = &hostConfig;
        guestContext.config = &guestConfig;
        if (withAudio) {
            hostContext.sounds = &hostSounds;
            guestContext.sounds = &guestSounds;
        }
        for (auto* context : {&hostContext, &guestContext}) {
            context->strings = &strings;
            context->levels = &catalog;
            context->unpackedRoot = root;
        }
        hostStore.hook = [](std::span<PartyMember> members) {
            REQUIRE(members.size() == 1);
            CHECK(members[0].player == kHostDevice);
            CHECK(members[0].slot == 22);
        };
        guestStore.hook = [](std::span<PartyMember> members) {
            REQUIRE(members.size() == 1);
            CHECK(members[0].player == kGuestDevice);
            CHECK(members[0].slot == 20);
        };
        const auto selected = [&](s32 device, s32 character, const std::string& name) {
            PartyMember result;
            result.player = device;
            result.slot = static_cast<usize>(20 + device);
            result.save.name = name;
            result.save.character = character;
            result.save.color = character;
            result.save.classUnlock = beatenLevel.empty() ? 0xFFFF : 0;
            result.save.moviesSeen = {"LOCAL"};
            result.save.classes[static_cast<usize>(character) + 2].experience = 123456;
            // Selected class is genuinely fresh: zero persisted health denotes
            // its default spawn health, not a fallen campaign continuation.
            REQUIRE(result.save.progress().health == 0);
            if (!beatenLevel.empty()) {
                const auto prior = catalog.byName(beatenLevel);
                REQUIRE(prior);
                result.save.progress().levels.recordBeaten(prior->realmId, prior->index);
                const auto gate = TowerAccess::kWorldGates[static_cast<usize>(prior->realmId)];
                REQUIRE(gate > 0);
                result.save.progress().crystals[static_cast<usize>(gate)] = -1;
            }
            return result;
        };
        std::vector<PartyMember> hostMembers{selected(kHostDevice, 0, "HOST")};
        if (secondHostPlayer) {
            hostMembers.push_back(selected(kSecondHostDevice, 2, "LOCAL"));
        }
        REQUIRE(hostParty.select(hostSession, hostMembers));
        REQUIRE(guestParty.select(guestSession, std::array{selected(kGuestDevice, 1, "GUEST")}));
        // The guest's first poll creates its connection event for the host's
        // next poll. Admission of the room is not yet authenticated readiness.
        for (u32 attempt = 0;
             attempt < 10 && (!hostSession.connected() || !guestSession.connected()); ++attempt) {
            pump();
        }
        REQUIRE(hostSession.connected());
        REQUIRE(guestSession.connected());
        const OnlineSession::NativeIdentity identity{"test-executable", "same-test-runtime",
                                                     std::string(64, 'a'), std::string(64, 'b'),
                                                     "en"};
        auto rules = OnlineSession::NativeRules{};
        rules.startupDelay = inputDelay;
        const auto initial = catalog.byName(initialLevel);
        REQUIRE(initial);
        const auto scene = NativeOnlinePlay::sceneId(catalog, *initial);
        REQUIRE(scene);
        rules.scene = *scene;
        REQUIRE(hostSession.configureNative(identity, rules));
        REQUIRE(guestSession.configureNative(identity));
        REQUIRE(hostSession.ready());
        REQUIRE(guestSession.ready());
        pump();
        REQUIRE(hostSession.start());
        for (u32 attempt = 0; attempt < 40; ++attempt) {
            pump();
            if (hostSession.nativeSetupPending()) {
                hostSetup = makeSetup(hostSession, hostConfig, options);
                REQUIRE(hostSession.confirmNativeSetup(hostSetup->digest()));
            }
            if (guestSession.nativeSetupPending()) {
                guestSetup = makeSetup(guestSession, guestConfig, options);
                REQUIRE(guestSession.confirmNativeSetup(guestSetup->digest()));
            }
            if (hostSession.phase() == OnlineSession::Phase::Active &&
                guestSession.phase() == OnlineSession::Phase::Active) {
                break;
            }
        }
        REQUIRE(hostSetup);
        REQUIRE(guestSetup);
        REQUIRE(hostSetup->digest() == guestSetup->digest());
        REQUIRE(hostSession.phase() == OnlineSession::Phase::Active);
        REQUIRE(guestSession.phase() == OnlineSession::Phase::Active);
        REQUIRE(hostSession.match().nativeInputs());
        REQUIRE(guestSession.match().nativeInputs());
        REQUIRE(guest.open(guestDevice, guestContext, guestSession, guestParty,
                           std::move(guestSetup), {&guestHaptics, &guestUnlocks, &guestStore}));
        REQUIRE(host.open(hostDevice, hostContext, hostSession, hostParty, std::move(hostSetup),
                          {&hostHaptics, &hostUnlocks, &hostStore}));
        for (u32 frame = 0; frame < 3600 && (host.phase() != NativeOnlinePlay::Phase::Playing ||
                                             guest.phase() != NativeOnlinePlay::Phase::Playing);
             ++frame) {
            step();
        }
        REQUIRE(host.phase() == NativeOnlinePlay::Phase::Playing);
        REQUIRE(guest.phase() == NativeOnlinePlay::Phase::Playing);
        REQUIRE(host.scene());
        REQUIRE(guest.scene());
    }

    static std::unique_ptr<NativePlaySetup>
    makeSetup(OnlineSession& session, const GameConfig& config, const PlayOptions& options) {
        REQUIRE(session.nativeContract());
        REQUIRE(session.party());
        REQUIRE(session.room());
        return test::agreedSetup(session, config, options);
    }
    void pump() {
        hostSession.update(0);
        guestSession.update(0);
        REQUIRE(hostSession.phase() != OnlineSession::Phase::Failed);
        REQUIRE(guestSession.phase() != OnlineSession::Phase::Failed);
    }
    static void draw(NativeOnlinePlay& play, test::FakeRenderDevice& device, f32 blend = 1) {
        play.render(device, Mat4{1}, 1280, 720, 1.0 / 60, blend);
        REQUIRE(play.phase() != NativeOnlinePlay::Phase::Failed);
    }
    void step(const SessionInputs::Frame& first = {}, const SessionInputs::Frame& second = {}) {
        INFO("host failure=" << static_cast<u32>(host.failure())
                             << " guest failure=" << static_cast<u32>(guest.failure()));
        auto hostInput = first;
        auto guestInput = second;
        // Exercise the ordinary unanimous movie-skip vote, not a test-only
        // entry bypass. Each peer votes solely through its owned device.
        hostInput[kHostDevice].movieSkipPressed =
            host.entryPhase() == OnlineLevelEntry::Phase::Movie;
        guestInput[kGuestDevice].movieSkipPressed =
            guest.entryPhase() == OnlineLevelEntry::Phase::Movie;
        REQUIRE(host.update(hostInput) != NativeOnlinePlay::Phase::Failed);
        REQUIRE(guest.update(guestInput) != NativeOnlinePlay::Phase::Failed);
        if (host.entryVisible()) {
            draw(host, hostDevice);
        }
        if (guest.entryVisible()) {
            draw(guest, guestDevice);
        }
    }
    void settle() {
        const auto controlsReady = [](const PlayScene& scene) {
            return std::ranges::all_of(scene.participants(), [](const PlayerRuntime& player) {
                return player.life != PlayerLife::Standing ||
                       (player.figure && !player.figure->animator().entering() &&
                        player.figure->animator().moveScale() > 0);
            });
        };
        for (u32 frame = 0; frame < 1200; ++frame) {
            REQUIRE(host.scene());
            REQUIRE(guest.scene());
            if (!host.scene()->spawning() && !guest.scene()->spawning() &&
                !host.scene()->startCamera().active() && !guest.scene()->startCamera().active() &&
                !host.scene()->scroll().active() && !guest.scene()->scroll().active() &&
                controlsReady(*host.scene()) && controlsReady(*guest.scene())) {
                return;
            }
            SessionInputs::Frame first;
            SessionInputs::Frame second;
            first[kHostDevice].menu.select = true;
            second[kGuestDevice].menu.select = true;
            step(first, second);
        }
        FAIL("native entry did not release gameplay within its bounded test window");
    }
    void synchronize() {
        // Identical fixture mutations must occur at the same native boundary,
        // not merely in the same application loop on differently paced peers.
        // Drain the most recent press edges first: subsequent zero predictions
        // must not rewind away out-of-band test fixture changes.
        for (u32 tick = 0; tick < 3; ++tick) {
            step();
        }
        pump();
        for (u32 attempt = 0; attempt < 30 && host.simulatedTick() != guest.simulatedTick();
             ++attempt) {
            auto& behind = host.simulatedTick() < guest.simulatedTick() ? host : guest;
            REQUIRE(behind.update({}) != NativeOnlinePlay::Phase::Failed);
        }
        REQUIRE(host.simulatedTick() == guest.simulatedTick());
    }
    static void placeOnPortal(NativeOnlinePlay& play, const std::string& destination = {},
                              bool secret = false) {
        REQUIRE(play.scene());
        const auto& exits = play.scene()->portals();
        usize found = 0;
        while (found < exits.size() &&
               (!exits.portal(found).destination ||
                (!destination.empty() && exits.portal(found).destination->name != destination) ||
                exits.portal(found).secret != secret)) {
            ++found;
        }
        REQUIRE(found < exits.size());
        REQUIRE_FALSE(exits.portal(found).shut);
        const auto position = exits.portal(found).position;
        for (const auto seat : kSeats) {
            REQUIRE(play.scene()->actor(seat));
            // Identical native contact fixture on each independent scene. No
            // transition/private flag is manufactured or network input changed.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            auto& actor = *const_cast<PlayerActor*>(play.scene()->actor(seat));
            actor.place(position);
        }
    }
};

PlayOptions immediateTower() {
    // The direct bootstrap fixture agrees these exact options on both peers;
    // menu sessions retain the default Tower setup.
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{0, 0, 0};
    return options;
}

struct NativeCommandTrace {
    std::array<std::map<u64, InputCommand>, InputCommand::kSeats> seats;

    void observe(std::span<const u8> bytes, u8 origin) {
        if (!RollbackExchange::recognizes(bytes)) {
            return;
        }
        const auto packet = RollbackExchange::decode(bytes);
        REQUIRE(packet);
        if (packet->origin != origin) {
            return;
        }
        for (const auto& command : packet->commands) {
            auto [found, inserted] = seats[command.seat].try_emplace(command.tick, command);
            if (!inserted) {
                // Pausing may discard a physical tap not yet published. It
                // must never rewrite a command already carried by the wire.
                CHECK(InputPacket::encode(std::array{found->second}) ==
                      InputPacket::encode(std::array{command}));
            }
        }
    }
    u64 latest(u8 seat) const { return seats[seat].empty() ? 0 : seats[seat].rbegin()->first; }
};

TEST_CASE("native online bootstrap opens canonical sparse fresh parties on both peers",
          "[native-online-play][native-bootstrap][assets]") {
    auto pair = std::make_unique<NativePair>();
    CHECK(pair->host.scene() != pair->guest.scene());
    CHECK(pair->host.scene()->world() != pair->guest.scene()->world());
    CHECK(pair->host.scene()->world()->ref().isTower());
    CHECK(pair->guest.scene()->world()->ref().isTower());
    for (const auto* play : {&pair->host, &pair->guest}) {
        REQUIRE(play->scene()->actorCount() == 2);
        CHECK_FALSE(play->scene()->actor(0));
        CHECK_FALSE(play->scene()->actor(2));
        for (const auto& member : play->scene()->participants()) {
            CHECK(std::ranges::find(kSeats, member.actor.player()) != kSeats.end());
            CHECK_FALSE(member.slot);
            CHECK(member.life == PlayerLife::Standing);
            CHECK(member.actor.save().health() > 0);
            CHECK(member.actor.save().classUnlock == 0);
            CHECK(member.actor.save().moviesSeen.empty());
            CHECK(member.actor.save()
                      .classes[static_cast<usize>(member.actor.save().character + 2)]
                      .experience == 0);
        }
    }
    CHECK(pair->hostParty.localMembers()[0].slot == 22);
    CHECK(pair->guestParty.localMembers()[0].slot == 20);
    CHECK(pair->hostParty.localMembers()[0].save.classUnlock == 0xFFFF);
    CHECK(pair->guestParty.localMembers()[0].save.classUnlock == 0xFFFF);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto* host = const_cast<PlayScene*>(pair->host.scene());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto* guest = const_cast<PlayScene*>(pair->guest.scene());
    std::map<u64, PlayScene::NativeDigestProfile> first;
    std::map<u64, PlayScene::NativeDigestProfile> second;
    u32 compared = 0;
    const auto observe = [&](NativeOnlinePlay& play, PlayScene& scene,
                             std::map<u64, PlayScene::NativeDigestProfile>& samples) {
        if (play.simulatedTick() > 0 && play.simulatedTick() % 30 == 0 &&
            !samples.contains(play.simulatedTick())) {
            PlayScene::NativeDigestProfile state;
            REQUIRE(scene.nativeAgreementDigest(&state));
            samples.emplace(play.simulatedTick(), std::move(state));
        }
    };
    for (u32 frame = 0; frame < 240; ++frame) {
        REQUIRE(pair->host.update({}) != NativeOnlinePlay::Phase::Failed);
        observe(pair->host, *host, first); // No host renders after entry.
        REQUIRE(pair->guest.update({}) != NativeOnlinePlay::Phase::Failed);
        pair->draw(pair->guest, pair->guestDevice, 0.25f);
        pair->draw(pair->guest, pair->guestDevice, 0.75f);
        observe(pair->guest, *guest, second);
    }
    for (const auto& [tick, state] : first) {
        if (const auto found = second.find(tick); found != second.end()) {
            CAPTURE(tick);
            CHECK(test::firstDivergence(state, found->second).empty());
            ++compared;
        }
    }
    CHECK(compared >= 6);
}

TEST_CASE("a guest catching up the shop hears only its newest steps' menu cues",
          "[native-online-play][native-shop]") {
    CHECK(NativeOnlinePlay::shopCuesHeard(true, 1000)); // the host steps the shop itself
    CHECK(NativeOnlinePlay::shopCuesHeard(false, 0));
    CHECK(NativeOnlinePlay::shopCuesHeard(false, NativeOnlinePlay::kHeardShopSteps - 1));
    CHECK_FALSE(NativeOnlinePlay::shopCuesHeard(false, NativeOnlinePlay::kHeardShopSteps));
}

TEST_CASE("a guest gone quiet is dropped by the host and its character waits in the tower",
          "[native-online-play][native-rollback-online][drop][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false, "L1", true);
    pair->settle();
    pair->synchronize();
    SessionInputs::Frame first;
    SessionInputs::Frame second;
    first[kHostDevice].move = {{1, 0}, 0.5f};
    second[kGuestDevice].move = {{-1, 0}, 0.5f};
    for (u32 frame = 0; frame < 30; ++frame) {
        pair->step(first, second);
    }
    // The guest's sends stop reaching the host: it predicts, then waits.
    pair->guestWire.silent = true;
    // What the guest had already sent arrives at the host's next poll; nothing comes after.
    REQUIRE(pair->host.update(first) != NativeOnlinePlay::Phase::Failed);
    const auto silentSince = MatchSession::Clock::now();
    for (u32 frame = 0; frame < 40; ++frame) {
        REQUIRE(pair->host.update(first) != NativeOnlinePlay::Phase::Failed);
        REQUIRE(pair->guest.update(second) != NativeOnlinePlay::Phase::Failed);
    }
    CHECK(pair->host.rollbackStatistics().stalls > 0);
    // Nothing fresh has come from it since: a second after that, it is quiet.
    const auto now = MatchSession::Clock::now();
    CHECK(pair->hostSession.match().quietSeconds(now) >=
          std::chrono::duration<f64>(now - silentSince).count());
    REQUIRE(pair->hostSession.match().quietPeers(MatchSession::kQuietAfter,
                                                 silentSince + std::chrono::seconds{1}) ==
            std::vector<u8>{2});
    REQUIRE(pair->hostSession.match().drop(2));
    const u64 before = pair->host.simulatedTick();
    for (u32 frame = 0; frame < 150; ++frame) {
        REQUIRE(pair->host.update(first) != NativeOnlinePlay::Phase::Failed);
        pair->guest.update(second); // told it is out; its session ends
    }
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->host.simulatedTick() > before + 100);
    CHECK(pair->host.confirmedTick() > before + 90);
    CHECK(pair->hostSession.match().quietPeers().empty());
    const auto* actor = pair->host.scene()->actor(kSeats[1]);
    REQUIRE(actor != nullptr);
    CHECK_FALSE(actor->moving());
    REQUIRE(pair->host.scene()->runtime(kSeats[1]) != nullptr);
    CHECK(pair->host.scene()->runtime(kSeats[1])->life == PlayerLife::InTower);
    CHECK(pair->host.scene()->actor(kSeats[0])->moving());
    CHECK(pair->guestSession.match().phase() == MatchSession::Phase::Stopped);
    CHECK(pair->guestSession.match().stopReason() == MatchStop::Left);
}

TEST_CASE("the remaining player leaves the tower once its guest is dropped",
          "[native-online-play][native-rollback-online][drop][native-travel][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false);
    pair->settle();
    pair->synchronize();
    const auto before = pair->hostSession.match().context().epoch;
    REQUIRE(pair->host.scene());
    const auto& exits = pair->host.scene()->portals();
    usize index = 0;
    while (index < exits.size() &&
           (!exits.portal(index).destination || exits.portal(index).destination->name != "G1")) {
        ++index;
    }
    REQUIRE(index < exits.size());
    REQUIRE_FALSE(exits.portal(index).shut);
    const Vec3 spot = exits.portal(index).position;
    // The guest's link closes after its queued messages arrive: the host drops it.
    pair->hostWire.events.push_back({PacketTransport::EventType::Disconnected, 1, {}, {}});
    for (u32 frame = 0; frame < 900 && pair->hostSession.match().context().epoch == before;
         ++frame) {
        // Only the host's own character stands on the portal.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        const_cast<PlayerActor*>(pair->host.scene()->actor(kSeats[0]))->place(spot);
        REQUIRE(pair->host.update({}) != NativeOnlinePlay::Phase::Failed);
    }
    CHECK(pair->hostSession.match().dropped(2));
    REQUIRE(pair->hostSession.match().context().epoch == before + 1);
    CHECK(pair->hostSession.match().context().transition == MatchTransition::Travel);
    // The guest's character waited in the tower and goes on with the party, fallen.
    REQUIRE(pair->host.scene());
    const auto* dropped = pair->host.scene()->runtime(kSeats[1]);
    REQUIRE(dropped != nullptr);
    CHECK(dropped->life == PlayerLife::InTower);
    const auto party = pair->host.scene()->party();
    const auto member = std::ranges::find(party, static_cast<s32>(kSeats[1]), &PartyMember::player);
    REQUIRE(member != party.end());
    CHECK(member->fallen);
}

TEST_CASE(
    "a guest whose simulation diverges rebuilds from the host's journal and the match goes on",
    "[native-online-play][native-rollback-online][journal][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false, "L1", true);
    pair->settle();
    pair->synchronize();
    for (u32 frame = 0; frame < 40; ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].move = {{1, 0}, 0.5f};
        second[kGuestDevice].move = {{-1, 0}, 0.5f};
        pair->step(first, second);
    }
    REQUIRE(pair->host.rollbackStatistics().nativeTicks > 0);
    // A divergence put straight into the guest's scene: its own character stands where the
    // host never saw it go, so the next confirmed digests disagree.
    // The scene has no door for a planted divergence; this is the one place that needs one.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): see above
    auto* actor = const_cast<PlayerActor*>(pair->guest.scene()->actor(kSeats[1]));
    REQUIRE(actor != nullptr);
    actor->place(actor->position() + Vec3{30, 0, 0});
    bool ordered = false;
    u32 rebuiltAt = 0;
    u64 orderedTick = 0;
    for (u32 frame = 0; frame < 600 && rebuiltAt == 0; ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].move = {{frame % 40 < 20 ? 1.0f : -1.0f, 0}, 0.5f};
        second[kGuestDevice].move = {{frame % 30 < 15 ? -1.0f : 1.0f, 0}, 0.5f};
        pair->step(first, second);
        if (!ordered && pair->hostSession.match().resyncing()) {
            ordered = true;
            orderedTick = pair->hostSession.match().resync()->tick;
            CHECK(pair->hostSession.match().resync()->peers.bits() == 0b100);
        }
        if (ordered && !pair->hostSession.match().resyncing() &&
            !pair->guestSession.match().resyncing()) {
            rebuiltAt = frame;
        }
    }
    REQUIRE(ordered);
    REQUIRE(rebuiltAt > 0);
    CHECK(orderedTick % MatchSession::kNativeAgreementPeriod == 0);
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->host.failure() == NativeOnlinePlay::Failure::None);
    CHECK(pair->guest.failure() == NativeOnlinePlay::Failure::None);
    CHECK(pair->guest.confirmedTick() >= orderedTick);
    // Both carry on and agree past the resync point.
    const u64 resumed = pair->guest.simulatedTick();
    for (u32 frame = 0; frame < 150; ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].move = {{frame % 40 < 20 ? 1.0f : -1.0f, 0}, 0.5f};
        second[kGuestDevice].move = {{frame % 30 < 15 ? -1.0f : 1.0f, 0}, 0.5f};
        pair->step(first, second);
    }
    CHECK(pair->guest.simulatedTick() > resumed + 100);
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->hostSession.match().nativeAgreement() != nullptr);
    CHECK(pair->hostSession.match().nativeAgreement()->agreedTick() > orderedTick);
    CHECK_FALSE(pair->hostSession.match().resyncing());
    CHECK_FALSE(pair->guestSession.match().resyncing());
}

TEST_CASE("a rebuilt guest keeps the input it sent before the host received it",
          "[native-online-play][native-rollback-online][journal][assets]") {
    NativeCommandTrace sent;
    u64 served = 0;
    std::optional<u64> sentWhenServed;
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false, "L1", true);
    pair->settle();
    pair->synchronize();
    // From here the guest's packets reach the host six frames late, so the host serves its
    // journal without the input the guest sent last. Nothing the guest sent may change.
    const auto guestPeer = pair->guestSession.match().localPeer();
    pair->guestWire.beforeSend = [&](std::span<const u8> bytes, auto) {
        sent.observe(bytes, guestPeer);
        return PeerTransport::SendResult::Sent;
    };
    pair->hostWire.beforeSend = [&](std::span<const u8> bytes, auto) {
        if (const auto chunk = JournalPacket::decode(bytes)) {
            for (const auto& command : chunk->commands) {
                if (command.seat == kSeats[1]) {
                    served = std::max(served, command.tick);
                }
            }
            if (chunk->last && !sentWhenServed) {
                sentWhenServed = sent.latest(kSeats[1]);
            }
        }
        return PeerTransport::SendResult::Sent;
    };
    pair->guestWire.latency = 6;
    for (u32 frame = 0; frame < 40; ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].move = {{1, 0}, 0.5f};
        second[kGuestDevice].move = {{-1, 0}, 0.5f};
        pair->step(first, second);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): a planted divergence, as above
    auto* actor = const_cast<PlayerActor*>(pair->guest.scene()->actor(kSeats[1]));
    REQUIRE(actor != nullptr);
    actor->place(actor->position() + Vec3{30, 0, 0});
    bool ordered = false;
    u32 rebuiltAt = 0;
    u64 orderedTick = 0;
    const auto play = [&](u32 frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].move = {{frame % 40 < 20 ? 1.0f : -1.0f, 0}, 0.5f};
        if (!ordered) {
            // Once the rebuild is ordered the guest lets go, so a tick it sampled again
            // would contradict the command it had already sent for it.
            second[kGuestDevice].move = {{frame % 30 < 15 ? -1.0f : 1.0f, 0}, 0.5f};
        }
        pair->step(first, second);
    };
    for (u32 frame = 0; frame < 600 && rebuiltAt == 0; ++frame) {
        play(frame);
        if (!ordered && pair->hostSession.match().resyncing()) {
            ordered = true;
            orderedTick = pair->hostSession.match().resync()->tick;
        }
        if (ordered && !pair->hostSession.match().resyncing() &&
            !pair->guestSession.match().resyncing()) {
            rebuiltAt = frame;
        }
    }
    REQUIRE(ordered);
    REQUIRE(rebuiltAt > 0);
    REQUIRE(sentWhenServed);
    CHECK(*sentWhenServed > served);
    CHECK(pair->guest.failure() == NativeOnlinePlay::Failure::None);
    // It carries on from the input it had sent, and both agree past the resync point.
    const u64 resumed = pair->guest.simulatedTick();
    for (u32 frame = 0; frame < 150; ++frame) {
        play(frame);
    }
    CHECK(pair->guest.simulatedTick() > resumed + 100);
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->hostSession.match().nativeAgreement() != nullptr);
    CHECK(pair->hostSession.match().nativeAgreement()->agreedTick() > orderedTick);
    CHECK_FALSE(pair->hostSession.match().resyncing());
    CHECK_FALSE(pair->guestSession.match().resyncing());
    pair->guestWire.beforeSend = {};
    pair->hostWire.beforeSend = {};
}

TEST_CASE("native online local pause fences only the owned device while agreed ticks continue",
          "[native-online-play][native-pause][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower());
    pair->settle();
    REQUIRE_FALSE(pair->host.pause(kGuestDevice));
    REQUIRE(pair->host.pause(kHostDevice));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Paused);
    const auto before = pair->host.simulatedTick();
    u32 checked = 0;
    for (u32 frame = 0; frame < 50; ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].move = {{1, 0}, 1};
        first[kHostDevice].attack = true;
        second[kGuestDevice].move = {{-1, 0}, 0.5f};
        pair->step(first, second);
        if (frame > 10) {
            for (const auto* play : {&pair->host, &pair->guest}) {
                const auto* stopped = play->scene()->actor(kSeats[0]);
                const auto* moving = play->scene()->actor(kSeats[1]);
                REQUIRE(stopped);
                REQUIRE(moving);
                CHECK_FALSE(stopped->moving());
                CHECK(moving->moving());
                ++checked;
            }
        }
    }
    CHECK(checked > 0);
    CHECK(pair->host.simulatedTick() >= before + 40);
    REQUIRE(pair->host.resume(kHostDevice));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK_FALSE(pair->host.resume(kHostDevice));
}

TEST_CASE("online rumble reaches only this machine's unpaused devices",
          "[native-online-play][rumble][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false, "L1", false, 1, true);
    pair->settle();
    REQUIRE(pair->hostParty.device(pair->hostSession, kSeats[0]) == kHostDevice);
    REQUIRE(pair->hostParty.device(pair->hostSession, kSecondHostSeat) == kSecondHostDevice);
    // Each machine presents every seat's rumble, as its ledger hands a tick's outputs on.
    const auto presentEverySeat = [&] {
        pair->hostHaptics.pulses.clear();
        pair->guestHaptics.pulses.clear();
        for (const auto* play : {&pair->host, &pair->guest}) {
            for (const u8 seat : {kSeats[0], kSecondHostSeat, kSeats[1]}) {
                REQUIRE(play->scene()->dispatchOutput(presented(seat)));
            }
        }
    };
    const auto damage = [](s32 device) {
        return rumble(device, kRumbleFrames, ControlFeedback::Damage);
    };
    presentEverySeat();
    CHECK(rumbles(pair->hostHaptics) ==
          std::vector<std::string>{damage(kHostDevice), damage(kSecondHostDevice)});
    CHECK(rumbles(pair->guestHaptics) == std::vector<std::string>{damage(kGuestDevice)});
    // A paused device feels nothing; the machine's other device still does. Pausing stops the
    // machine's motors.
    const s32 stopsBeforePause = pair->hostHaptics.stops;
    REQUIRE(pair->host.pause(kHostDevice));
    CHECK(pair->hostHaptics.stops == stopsBeforePause + 1);
    presentEverySeat();
    CHECK(rumbles(pair->hostHaptics) == std::vector<std::string>{damage(kSecondHostDevice)});
    CHECK(rumbles(pair->guestHaptics) == std::vector<std::string>{damage(kGuestDevice)});
    REQUIRE(pair->host.resume(kHostDevice));
    presentEverySeat();
    CHECK(rumbles(pair->hostHaptics) ==
          std::vector<std::string>{damage(kHostDevice), damage(kSecondHostDevice)});
    // Closing stops them as the scene's sounds pause, as the game quiets and as the scene
    // closes; a closed game no longer borrows this machine's controllers.
    const s32 stopsBeforeClose = pair->hostHaptics.stops;
    pair->host.close();
    CHECK(pair->hostHaptics.stops == stopsBeforeClose + 3);
    pair->host.close();
    CHECK(pair->hostHaptics.stops == stopsBeforeClose + 3);
}

TEST_CASE(
    "online console permission routes guest mutations to its canonical seat and revokes requests",
    "[native-online-play][online-console][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower());
    pair->settle();
    auto& host = pair->hostSession.match();
    auto& guest = pair->guestSession.match();
    CHECK_FALSE(host.consolePermissions());
    CHECK_FALSE(guest.consolePermissions());
    CHECK_FALSE(guest.setConsolePermissions(true));
    CHECK_FALSE(guest.requestConsole({kSeats[1], "get gold 321"}));
    REQUIRE(host.setConsolePermissions(true));
    for (u32 frame = 0; frame < 10 && !guest.consolePermissions(); ++frame) {
        pair->step();
    }
    REQUIRE(guest.consolePermissions());
    CHECK_FALSE(guest.requestConsole({kSeats[0], "get gold 999"}));
    CHECK_FALSE(guest.requestConsole({kSeats[1], "console_permissions 0"}));

    const s32 hostGold = pair->host.scene()->actor(kSeats[0])->save().gold;
    const s32 guestGold = pair->guest.scene()->actor(kSeats[1])->save().gold;
    const usize placed = pair->host.world()->placedItems().size();
    REQUIRE(guest.requestConsole({kSeats[1], "get gold 321"}));
    REQUIRE(guest.requestConsole({kSeats[1], "god"}));
    REQUIRE(guest.requestConsole({kSeats[1], "spawn item KEY"}));
    for (u32 frame = 0; frame < 60; ++frame) {
        pair->step();
    }
    for (const auto* play : {&pair->host, &pair->guest}) {
        REQUIRE(play->scene());
        CHECK(play->scene()->actor(kSeats[0])->save().gold == hostGold);
        CHECK(play->scene()->actor(kSeats[1])->save().gold == guestGold + 321);
        CHECK_FALSE(play->scene()->cheats(kSeats[0]).god);
        CHECK(play->scene()->cheats(kSeats[1]).god);
        CHECK(play->world()->placedItems().size() == placed + 1);
    }
    CHECK(pair->host.takeConsoleReplies().empty());
    CHECK(pair->guest.takeConsoleReplies() ==
          std::vector<std::string>{"Got 321 gold", "god on", "Dropped KEY"});
    CHECK(pair->guest.takeConsoleReplies().empty());

    // The console captures the owner's devices, not shared time or remote controls.
    pair->guest.consoleCapture(true);
    const auto tick = pair->host.simulatedTick();
    SessionInputs::Frame first;
    SessionInputs::Frame second;
    first[kHostDevice].move = {{1, 0}, 0.5f};
    second[kGuestDevice].move = {{-1, 0}, 0.5f};
    for (u32 frame = 0; frame < 30; ++frame) {
        pair->step(first, second);
    }
    CHECK(pair->host.simulatedTick() > tick + 20);
    CHECK(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->host.scene()->actor(kSeats[0])->moving());
    CHECK_FALSE(pair->host.scene()->actor(kSeats[1])->moving());
    CHECK_FALSE(pair->guest.scene()->actor(kSeats[1])->moving());
    pair->guest.consoleCapture(false);

    // Admission by the host and execution by the simulation are separate. A
    // revocation before sampling cancels even an already delivered guest request.
    REQUIRE(guest.requestConsole({kSeats[1], "notarget"}));
    pair->guestSession.update(0);
    pair->hostSession.update(0);
    REQUIRE(host.setConsolePermissions(false));
    for (u32 frame = 0; frame < 60; ++frame) {
        pair->step();
    }
    CHECK_FALSE(guest.consolePermissions());
    CHECK_FALSE(guest.requestConsole({kSeats[1], "god"}));
    CHECK_FALSE(pair->host.scene()->cheats(kSeats[1]).noTarget);
    CHECK_FALSE(pair->guest.scene()->cheats(kSeats[1]).noTarget);
    CHECK(pair->guest.takeConsoleReplies().empty());
    // Revocation does not undo a previously confirmed command, or deny the host.
    CHECK(pair->guest.scene()->cheats(kSeats[1]).god);
    REQUIRE(host.requestConsole({kSeats[0], "get gold 27"}));
    for (u32 frame = 0; frame < 30; ++frame) {
        pair->step();
    }
    CHECK(pair->host.scene()->actor(kSeats[0])->save().gold == hostGold + 27);
    CHECK(pair->guest.scene()->actor(kSeats[0])->save().gold == hostGold + 27);
    CHECK(pair->host.takeConsoleReplies().size() == 1);
    CHECK(pair->guest.takeConsoleReplies().empty());
}

TEST_CASE("guest console travel and tower unlock share the confirmed online load boundary",
          "[native-online-play][online-console][native-travel][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false);
    pair->settle();
    REQUIRE(pair->hostSession.match().setConsolePermissions(true));
    for (u32 frame = 0; frame < 10 && !pair->guestSession.match().consolePermissions(); ++frame) {
        pair->step();
    }
    REQUIRE(pair->guestSession.match().consolePermissions());
    const auto travel = [&](std::string_view line, std::string_view destination) {
        const auto epoch = pair->hostSession.match().context().epoch;
        REQUIRE(pair->guestSession.match().requestConsole({kSeats[1], std::string{line}}));
        for (u32 frame = 0; frame < 3600; ++frame) {
            pair->step();
            if (pair->hostSession.match().context().epoch > epoch &&
                pair->guestSession.match().context().epoch > epoch &&
                pair->host.phase() == NativeOnlinePlay::Phase::Playing &&
                pair->guest.phase() == NativeOnlinePlay::Phase::Playing) {
                break;
            }
        }
        REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
        REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
        const auto& context = pair->hostSession.match().context();
        REQUIRE(context.epoch > epoch);
        CHECK(context.sourceEpoch == epoch);
        CHECK(context.sourceTick > 0);
        CHECK(context == pair->guestSession.match().context());
        REQUIRE(pair->host.world());
        REQUIRE(pair->guest.world());
        CHECK(pair->host.world()->ref().name == destination);
        CHECK(pair->guest.world()->ref().name == destination);
        CHECK(pair->host.shopScene() == nullptr);
        CHECK(pair->guest.shopScene() == nullptr);
        CHECK(pair->hostSession.match().consolePermissions());
        CHECK(pair->guestSession.match().consolePermissions());
        pair->settle();
    };
    REQUIRE(pair->guestSession.match().requestConsole({kSeats[1], "god"}));
    for (u32 frame = 0; frame < 30; ++frame) {
        pair->step();
    }
    travel("goto G1", "G1");
    const auto* hostParent = pair->host.scene();
    const auto* guestParent = pair->guest.scene();
    CHECK(hostParent->cheats(kSeats[1]).god);
    CHECK(guestParent->cheats(kSeats[1]).god);
    travel("goto S4", "S4");
    CHECK(pair->hostSession.match().context().transition == MatchTransition::SecretEnter);
    CHECK(pair->host.scene() != hostParent);
    CHECK(pair->guest.scene() != guestParent);
    CHECK(pair->host.scene()->cheats(kSeats[1]).god);
    CHECK(pair->guest.scene()->cheats(kSeats[1]).god);
    REQUIRE(pair->guestSession.match().requestConsole({kSeats[1], "god"}));
    REQUIRE(pair->guestSession.match().requestConsole({kSeats[1], "notarget"}));
    for (u32 frame = 0; frame < 30; ++frame) {
        pair->step();
    }
    travel("goto S1", "S1");
    CHECK(pair->hostSession.match().context().transition == MatchTransition::Travel);
    for (const auto* play : {&pair->host, &pair->guest}) {
        CHECK_FALSE(play->scene()->cheats(kSeats[1]).god);
        CHECK(play->scene()->cheats(kSeats[1]).noTarget);
    }
    travel("goto G1", "G1");
    CHECK(pair->hostSession.match().context().transition == MatchTransition::SecretReturn);
    CHECK_FALSE(pair->hostSession.match().context().challengeWon);
    CHECK(pair->host.scene() == hostParent);
    CHECK(pair->guest.scene() == guestParent);
    for (const auto* play : {&pair->host, &pair->guest}) {
        CHECK_FALSE(play->scene()->consoleTravelRequest());
        CHECK_FALSE(play->scene()->secretTravel());
        CHECK_FALSE(play->scene()->cheats(kSeats[1]).god);
        CHECK(play->scene()->cheats(kSeats[1]).noTarget);
    }
    const auto returnedEpoch = pair->hostSession.match().context().epoch;
    for (u32 frame = 0; frame < 30; ++frame) {
        pair->step();
    }
    CHECK(pair->hostSession.match().context().epoch == returnedEpoch);
    CHECK(pair->guestSession.match().context().epoch == returnedEpoch);
    // Leaving a challenge for a different normal map discards its suspended
    // parent instead of trying to resume it or awarding the challenge reward.
    travel("goto S4", "S4");
    travel("goto G2", "G2");
    CHECK(pair->hostSession.match().context().transition == MatchTransition::Travel);
    travel("goto tower", "L1");
    travel("unlock_tower", "L1");
    for (const auto* play : {&pair->host, &pair->guest}) {
        for (const auto seat : kSeats) {
            REQUIRE(play->scene()->actor(seat));
            const auto& relics = play->scene()->actor(seat)->save().progress().relics;
            CHECK(relics.runeCount() == Relics::kRuneCount);
            CHECK(relics.pendingRunes == 0);
            CHECK(relics.pendingShards == 0);
        }
    }
    CHECK(pair->hostStore.autoSaves == 0);
    CHECK(pair->guestStore.autoSaves == 0);
    CHECK(pair->hostUnlocks.masks.empty());
    CHECK(pair->guestUnlocks.masks.empty());
}

TEST_CASE("shipping native online predicts local movement before delayed peer inputs arrive",
          "[native-online-play][native-rollback-online][native-latency][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false, "L1", true);
    pair->settle();
    pair->synchronize();
    const auto epoch = pair->hostSession.match().context().epoch;
    for (const auto* play : {&pair->host, &pair->guest}) {
        for (const auto& player : play->scene()->participants()) {
            REQUIRE(player.figure);
            REQUIRE_FALSE(player.figure->animator().entering());
            REQUIRE(player.figure->animator().moveScale() > 0);
        }
    }
    REQUIRE(pair->host.scene()->recordedAudioEpoch() == epoch);
    REQUIRE(pair->guest.scene()->recordedAudioEpoch() == epoch);
    pair->hostWire.latency = pair->guestWire.latency = 4;
    usize hostAudio = 0;
    usize guestAudio = 0;
    const SoundPlayer::Capture captureHost(&pair->hostSounds, [&](const auto&) { ++hostAudio; });
    const SoundPlayer::Capture captureGuest(&pair->guestSounds, [&](const auto&) { ++guestAudio; });
    std::optional<u32> locallyMoving;
    std::optional<u32> remotelyMoving;
    u64 largestSpeculation = 0;
    for (u32 frame = 0; frame < 120; ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        if (frame >= 12) {
            first[kHostDevice].move = {{frame % 24 < 12 ? 1.0f : -1.0f, 0}, 0.5f};
        }
        second[kGuestDevice].move = {{frame % 20 < 10 ? -1.0f : 1.0f, 0}, 0.5f};
        if (frame >= 20) {
            first[kHostDevice].attack = second[kGuestDevice].attack = frame % 20 < 10;
            first[kHostDevice].attackPressed = second[kGuestDevice].attackPressed = frame % 20 == 0;
        }
        pair->step(first, second);
        if (!locallyMoving && pair->guest.scene()->actor(kSeats[1])->moving()) {
            locallyMoving = frame;
        }
        if (!remotelyMoving && pair->host.scene()->actor(kSeats[1])->moving()) {
            remotelyMoving = frame;
        }
        for (const auto* play : {&pair->host, &pair->guest}) {
            REQUIRE(play->confirmedTick() <= play->simulatedTick());
            const u64 speculative = play->simulatedTick() - play->confirmedTick();
            largestSpeculation = std::max(largestSpeculation, speculative);
            CHECK(speculative <= RollbackInputs::kPredictionTicks);
            CHECK(play->rollbackStatistics().maxReplay <= RollbackInputs::kPredictionTicks);
        }
        // Audio device work remains outside replay; real captured calls must
        // reach both outputs without requiring either peer to render.
        std::array<f32, 1600> mixed{};
        pair->hostMixer.mix(mixed);
        pair->guestMixer.mix(mixed);
        pair->hostSounds.update();
        pair->guestSounds.update();
    }
    REQUIRE(locallyMoving);
    REQUIRE(remotelyMoving);
    INFO("local motion application frame=" << *locallyMoving
                                           << " remote presentation frame=" << *remotelyMoving);
    CHECK(*locallyMoving <= pair->guestSession.match().context().inputLead + 1U);
    CHECK(*locallyMoving < *remotelyMoving);
    CHECK(largestSpeculation >= 2);
    CHECK(pair->host.rollbackStatistics().corrections > 0);
    CHECK(pair->guest.rollbackStatistics().corrections > 0);
    CHECK(hostAudio > 0);
    CHECK(guestAudio > 0);
    CHECK(pair->hostStore.autoSaves == 0);
    CHECK(pair->guestStore.autoSaves == 0);
    CHECK(pair->hostUnlocks.masks.empty());
    CHECK(pair->guestUnlocks.masks.empty());
    pair->hostWire.latency = pair->guestWire.latency = 0;
    for (u32 frame = 0; frame < 60; ++frame) {
        pair->step();
    }
    REQUIRE(pair->hostSession.match().nativeAgreement());
    REQUIRE(pair->guestSession.match().nativeAgreement());
    CHECK_FALSE(pair->hostSession.match().nativeAgreement()->failed());
    CHECK_FALSE(pair->guestSession.match().nativeAgreement()->failed());
    CHECK(pair->hostSession.match().nativeAgreement()->agreedTick() >= 120);
    CHECK(pair->guestSession.match().nativeAgreement()->agreedTick() >= 120);
}

TEST_CASE("shipping correction admits current local input without an extra application update",
          "[native-online-play][native-pending-input][native-latency][assets]") {
    const u8 delay = GENERATE(u8{0}, u8{1});
    NativeCommandTrace published;
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false, "L1", false, delay);
    pair->settle();
    pair->synchronize();
    pair->guestWire.beforeSend = [&](std::span<const u8> bytes, auto) {
        published.observe(bytes, pair->guestSession.match().localPeer());
        return PeerTransport::SendResult::Sent;
    };
    pair->hostWire.latency = pair->guestWire.latency = 3;
    u32 checkedCorrections = 0;
    for (u32 frame = 0; frame < 72; ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].move = {{frame % 4 < 2 ? 1.0f : -1.0f, 0}, 0.5f};
        second[kGuestDevice].move = {{frame % 2 == 0 ? -1.0f : 1.0f, 0}, 0.5f};
        const auto latest = published.latest(kSeats[1]);
        const auto nextSample = pair->guest.simulatedTick() + 1 + delay;
        const auto before = pair->guest.rollbackStatistics().corrections;
        // NativeOnlinePlay samples before advance(), unlike a harness that
        // performs a separate correction pre-pass before polling controls.
        pair->step(first, second);
        if (pair->guest.rollbackStatistics().corrections != before && nextSample > latest) {
            CAPTURE(delay, frame, nextSample, latest);
            const auto& seat = published.seats[kSeats[1]];
            REQUIRE(seat.contains(nextSample));
            CHECK(seat.at(nextSample).direction == second[kGuestDevice].move.direction);
            CHECK(seat.at(nextSample).magnitude == second[kGuestDevice].move.magnitude);
            if (delay == 0) {
                CHECK(pair->guest.simulatedTick() >= nextSample);
            }
            ++checkedCorrections;
        }
    }
    CHECK(checkedCorrections >= 8);
    pair->hostWire.latency = pair->guestWire.latency = 0;
    for (u32 frame = 0; frame < 30; ++frame) {
        pair->step();
    }
    REQUIRE(pair->guestSession.match().nativeAgreement());
    CHECK_FALSE(pair->guestSession.match().nativeAgreement()->failed());
    pair->guestWire.beforeSend = {};
}

TEST_CASE("shipping renders predicted local movement with only the native interpolation interval",
          "[native-online-play][native-latency][native-visible-input][assets]") {
    const u8 delay = GENERATE(u8{0}, u8{1});
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false, "L1", false, delay);
    pair->settle();
    pair->synchronize();
    pair->hostWire.latency = pair->guestWire.latency = 4;
    const auto* scene = pair->guest.scene();
    REQUIRE(scene);
    const auto local =
        std::ranges::find(scene->participants(), kSeats[1],
                          [](const PlayerRuntime& player) { return player.actor.player(); });
    REQUIRE(local != scene->participants().end());
    REQUIRE(local->figure);
    REQUIRE_FALSE(local->transport.active());
    REQUIRE_FALSE(local->capture.body());
    const Vec3 initial = local->actor.position();
    const Vec3 remoteInitial = pair->host.scene()->actor(kSeats[1])->position();
    WorldCamera previousCamera;
    std::optional<u32> movementFrame;
    for (u32 frame = 0; frame < 3; ++frame) {
        SessionInputs::Frame input;
        input[kGuestDevice].move = {{-1, 0}, 0.5f};
        previousCamera = scene->viewCamera();
        pair->step({}, input);
        if (local->actor.position() != initial) {
            movementFrame = frame;
            break;
        }
    }
    REQUIRE(movementFrame);
    CHECK(*movementFrame <= delay + 1U);
    // The display under test belongs to the predicting guest. The host has not
    // received this movement yet, so a host echo cannot satisfy the assertions.
    CHECK(pair->host.scene()->actor(kSeats[1])->position() == remoteInitial);
    CHECK(pair->guest.simulatedTick() > pair->guest.confirmedTick());
    REQUIRE(local->previous.continuous);
    REQUIRE(local->previous.position == initial);
    REQUIRE(local->figure->animationRevision() != local->previous.animationRevision);
    const Vec3 current = local->actor.position();
    const auto currentCamera = scene->viewCamera();
    const auto nativeTick = pair->guest.simulatedTick();
    const auto corrections = pair->guest.rollbackStatistics().corrections;
    const auto worn = PowerupEffects::of(local->actor.save().progress().inventory);
    test::FakeRenderDevice reference;
    pair->guestDevice.recordDraws = true;

    const auto samePositions = [](const test::RecordedDraw& a, const test::RecordedDraw& b) {
        return a.texture == b.texture && a.vertices.size() == b.vertices.size() &&
               std::ranges::equal(a.vertices, b.vertices, {}, &ImmediateVertex::position,
                                  &ImmediateVertex::position);
    };
    for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
        CAPTURE(delay, *movementFrame, alpha);
        pair->guestDevice.draws.clear();
        pair->draw(pair->guest, pair->guestDevice, alpha);
        REQUIRE_FALSE(pair->guestDevice.draws.empty());

        // Derive the expected body directly from the two actual native states,
        // not from PartyFigures::presentationBody (the code under test). Mesh
        // vertices are emitted in world space; RecordedDraw::transform is the
        // camera's clip matrix, not the player's body transform.
        const Vec3 position = glm::mix(initial, current, alpha);
        const f32 yaw = local->previous.yaw +
                        std::remainder(local->actor.yaw() - local->previous.yaw, kTwoPi) * alpha;
        const Mat4 base = glm::rotate(glm::translate(Mat4{1}, position), yaw, Vec3{0, 1, 0});
        const Mat4 body = PlayerFigure::bodyPlacement(base, local->actor.save(), worn);
        const auto camera = currentCamera.interpolate(previousCamera, alpha);
        const auto frame = CameraFrame::of(camera);
        const Mat4 clip =
            camera.clipTransform(pair->guestConfig.horizontalFovRadians(), 1280, 720, Mat4{1});
        reference.draws.clear();
        local->figure->draw(reference, clip, body, scene->world()->lighting(), 1,
                            local->move.weaponHidden(), &frame, alpha);
        REQUIRE_FALSE(reference.draws.empty());
        for (const auto& expected : reference.draws) {
            REQUIRE_FALSE(expected.vertices.empty());
            const auto actual =
                std::ranges::find_if(pair->guestDevice.draws, [&](const auto& draw) {
                    return samePositions(draw, expected);
                });
            REQUIRE(actual != pair->guestDevice.draws.end());
            CHECK(actual->transform == clip);
        }

        // Hold the same animation pose and yaw at the old position. At alpha
        // .5/1 the actual emitted costume must already have left it, without
        // another update or a packet arriving. Alpha0 intentionally retains it.
        const auto predicted = reference.draws;
        const Mat4 oldBase = glm::rotate(glm::translate(Mat4{1}, initial), yaw, Vec3{0, 1, 0});
        reference.draws.clear();
        local->figure->draw(
            reference, clip, PlayerFigure::bodyPlacement(oldBase, local->actor.save(), worn),
            scene->world()->lighting(), 1, local->move.weaponHidden(), &frame, alpha);
        REQUIRE(reference.draws.size() == predicted.size());
        CHECK(samePositions(reference.draws.front(), predicted.front()) == (alpha == 0));
        CHECK(pair->guest.simulatedTick() == nativeTick);
        CHECK(pair->guest.rollbackStatistics().corrections == corrections);
        CHECK(pair->host.scene()->actor(kSeats[1])->position() == remoteInitial);
    }
}

TEST_CASE("native pause discards only one local device's unpublished stalled tap",
          "[native-online-play][native-pending-input][native-pause][assets]") {
    const u8 delay = GENERATE(u8{0}, u8{1});
    NativeCommandTrace published;
    auto pair = std::make_unique<NativePair>(immediateTower(), "", false, "L1", false, delay, true);
    pair->settle();
    pair->synchronize();
    REQUIRE(pair->hostParty.device(pair->hostSession, kSeats[0]) == kHostDevice);
    REQUIRE(pair->hostParty.device(pair->hostSession, kSecondHostSeat) == kSecondHostDevice);
    pair->hostWire.beforeSend = [&](std::span<const u8> bytes, auto) {
        published.observe(bytes, pair->hostSession.match().localPeer());
        return PeerTransport::SendResult::Sent;
    };
    bool blocked = true;
    pair->guestWire.beforeSend = [&](std::span<const u8> bytes, auto) {
        return blocked && RollbackExchange::recognizes(bytes) ? PeerTransport::SendResult::Congested
                                                              : PeerTransport::SendResult::Sent;
    };
    u32 held = 0;
    for (u32 frame = 0; frame < 40 && held < 3; ++frame) {
        const auto before = pair->host.simulatedTick();
        SessionInputs::Frame second;
        second[kGuestDevice].move = {{-1, 0}, 0.5f};
        pair->step({}, second);
        held = pair->host.simulatedTick() == before ? held + 1 : 0;
    }
    REQUIRE(held == 3);
    const auto stalled = pair->host.simulatedTick();
    const auto lastPublished = published.latest(kSeats[0]);
    REQUIRE(lastPublished > 0);
    REQUIRE(published.latest(kSecondHostSeat) == lastPublished);
    SessionInputs::Frame taps;
    for (const auto device : {kHostDevice, kSecondHostDevice}) {
        taps[device].attack = true;
        taps[device].attackPressed = true;
    }
    pair->step(taps);
    REQUIRE(pair->host.simulatedTick() == stalled);
    REQUIRE(published.latest(kSeats[0]) == lastPublished);
    REQUIRE(pair->host.pause(kHostDevice));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Paused);
    // The application keeps polling while the menu is open. Neutral devices
    // must not accidentally reintroduce a consumed tap from the pending queue.
    pair->step();
    pair->step();
    blocked = false;
    for (u32 frame = 0; frame < 40 && published.latest(kSeats[0]) == lastPublished; ++frame) {
        pair->step();
    }
    const u64 firstAfterStall = lastPublished + 1;
    REQUIRE(published.seats[kSeats[0]].contains(firstAfterStall));
    REQUIRE(published.seats[kSecondHostSeat].contains(firstAfterStall));
    const auto& paused = published.seats[kSeats[0]].at(firstAfterStall);
    const auto& continuing = published.seats[kSecondHostSeat].at(firstAfterStall);
    CHECK(paused.heldButtons == 0);
    CHECK(paused.pressedButtons == 0);
    CHECK(paused.direction == Vec2{0});
    CHECK(paused.magnitude == 0);
    CHECK_FALSE(paused.aimDirection);
    CHECK(paused.hintTopic == 0);
    CHECK(paused.hintRevision == 0);
    CHECK(continuing.held(CommandHeld::Attack));
    CHECK(continuing.pressed(CommandPress::Attack));
    CHECK(pair->host.rollbackStatistics().corrections > 0);
    REQUIRE(pair->host.resume(kHostDevice));
    for (u32 frame = 0; frame < 24; ++frame) {
        pair->step();
    }
    for (const auto& [tick, command] : published.seats[kSeats[0]]) {
        if (tick > lastPublished) {
            CHECK_FALSE(command.pressed(CommandPress::Attack));
        }
    }
    CHECK(pair->host.simulatedTick() > stalled);
    CHECK(pair->hostStore.autoSaves == 0);
    CHECK(pair->guestStore.autoSaves == 0);
    pair->hostWire.beforeSend = {};
    pair->guestWire.beforeSend = {};
}

TEST_CASE("native online travel drains the terminal tick and retains the native shop party",
          "[native-online-play][native-travel][native-shop][assets]") {
    const bool autoSave = GENERATE(false, true);
    auto pair = std::make_unique<NativePair>(immediateTower(), "", autoSave);
    CHECK_FALSE(pair->host.hasCompletedProgress());
    CHECK_FALSE(pair->guest.hasCompletedProgress());
    CHECK_FALSE(pair->host.saveCompletedProgress());
    CHECK_FALSE(pair->guest.saveCompletedProgress());
    CHECK(pair->hostStore.saves == 0);
    CHECK(pair->guestStore.saves == 0);
    pair->settle();
    pair->synchronize();
    const auto epoch = pair->hostSession.match().context().epoch;
    bool observedDrain = false;
    u64 sourceTick = 0;
    for (u32 frame = 0; frame < 1800 && pair->hostSession.match().context().epoch == epoch;
         ++frame) {
        pair->placeOnPortal(pair->host, "G1");
        pair->placeOnPortal(pair->guest, "G1");
        REQUIRE(pair->host.update({}) != NativeOnlinePlay::Phase::Failed);
        if (pair->hostSession.match().context().epoch != epoch) {
            const auto& next = pair->hostSession.match().context();
            REQUIRE(next.transition == MatchTransition::Travel);
            REQUIRE(next.sourceEpoch == epoch);
            sourceTick = next.sourceTick;
            CHECK(sourceTick == pair->host.simulatedTick());
            CHECK(sourceTick == pair->host.confirmedTick());
            const auto before = pair->guest.simulatedTick();
            const auto confirmedBefore = pair->guest.confirmedTick();
            // Poll control and source input without stepping the scene. The
            // authenticated Prepare must not replace an unexecuted old world.
            pair->guestSession.update(0);
            if (pair->guestSession.match().context().epoch == epoch) {
                REQUIRE(pair->guestSession.match().nativeDraining());
            } else {
                // Prediction may have reached and confirmed the same terminal
                // boundary first. Only that completed boundary permits the
                // Prepare to adopt without another native step.
                CHECK(confirmedBefore == sourceTick);
                CHECK(pair->guest.outcome() == NativeRollbackDriver::Status::Travel);
                CHECK(pair->guestSession.match().context().sourceTick == sourceTick);
            }
            CHECK(pair->guest.simulatedTick() == before);
            CHECK(pair->guest.scene()->world()->ref().isTower());
            CHECK(before <= sourceTick);
            observedDrain = true;
        }
        REQUIRE(pair->guest.update({}) != NativeOnlinePlay::Phase::Failed);
    }
    REQUIRE(observedDrain);
    REQUIRE(sourceTick > 0);
    REQUIRE(pair->guest.simulatedTick() == sourceTick);
    REQUIRE(pair->guestSession.match().context().sourceTick == sourceTick);
    REQUIRE(pair->host.pause(kHostDevice));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Loading);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        pair->step();
        if (pair->host.phase() == NativeOnlinePlay::Phase::Paused) {
            // The local overlay survives resource loading/Commit and continues
            // to suppress its device until the explicit Resume action.
            REQUIRE(pair->host.resume(kHostDevice));
        }
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->host.scene());
    REQUIRE(pair->guest.scene());
    CHECK(pair->host.scene()->world()->ref().name == "G1");
    CHECK(pair->guest.scene()->world()->ref().name == "G1");
    // Leaving the tower beats nothing, and only the tower is told what was beaten.
    CHECK_FALSE(pair->host.scene()->levelBeaten().has_value());
    CHECK_FALSE(pair->guest.scene()->levelBeaten().has_value());
    const LevelRef fields = pair->host.scene()->world()->ref();
    CHECK(pair->hostSession.match().context() == pair->guestSession.match().context());
    for (const auto seat : kSeats) {
        CHECK(pair->host.scene()->actor(seat)->save().name ==
              pair->guest.scene()->actor(seat)->save().name);
        CHECK_FALSE(pair->host.scene()->party()[seat == 1 ? 0 : 1].slot);
        CHECK_FALSE(pair->guest.scene()->party()[seat == 1 ? 0 : 1].slot);
    }

    pair->settle();
    pair->synchronize();
    // Mutate the same legitimate native fixture on each peer. CharacterProfile
    // deliberately omits these transient fields: rebuilding from it at travel
    // would lose help history and the accumulated results checkpoint. Native
    // PartyRecords resets turbo at a level boundary; retain that rule too.
    for (const auto* play : {&pair->host, &pair->guest}) {
        for (const auto& member : play->scene()->participants()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            auto& runtime = const_cast<PlayerRuntime&>(member);
            runtime.actor.save().gold = 10000 + runtime.actor.player();
            runtime.turbo.add(37.0f);
            runtime.helpHeard = {HelpMessages::kFirstTurboName};
            runtime.levelKills = runtime.actor.player() + 2;
        }
    }
    const auto sourceEpoch = pair->hostSession.match().context().epoch;
    for (u32 frame = 0; frame < 1800 && (pair->host.phase() != NativeOnlinePlay::Phase::Shopping ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Shopping);
         ++frame) {
        // Normal G1 tags name its next stage. Native leaveBy redirects that
        // journey to the Tower through the tally/shop; do not invent a tag.
        if (pair->host.scene() && pair->host.scene()->world()->ref().name == "G1") {
            pair->placeOnPortal(pair->host);
        }
        if (pair->guest.scene() && pair->guest.scene()->world()->ref().name == "G1") {
            pair->placeOnPortal(pair->guest);
        }
        pair->step();
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Shopping);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Shopping);
    REQUIRE(pair->host.shopScene());
    REQUIRE(pair->guest.shopScene());
    CHECK(pair->hostSession.match().context() == pair->guestSession.match().context());
    CHECK(pair->hostSession.match().context().transition == MatchTransition::Shop);
    CHECK(pair->hostSession.match().context().sourceEpoch == sourceEpoch);
    CHECK(pair->hostSession.match().context().sourceTick > 0);
    CHECK(pair->hostStore.autoSaves == 0);
    CHECK(pair->guestStore.autoSaves == 0);
    const auto hostMembers = pair->host.shopScene()->session().party();
    const auto guestMembers = pair->guest.shopScene()->session().party();
    REQUIRE(hostMembers.size() == kSeats.size());
    REQUIRE(guestMembers.size() == kSeats.size());
    for (usize i = 0; i < kSeats.size(); ++i) {
        const auto& host = hostMembers[i];
        const auto& guest = guestMembers[i];
        CAPTURE(i, host.player, guest.player);
        CHECK(host.player == kSeats[i]);
        CHECK(guest.player == kSeats[i]);
        CHECK_FALSE(host.slot);
        CHECK_FALSE(guest.slot);
        CHECK(host.fallen == guest.fallen);
        CHECK(host.turbo == 0);
        CHECK(host.turbo == guest.turbo);
        CHECK(std::ranges::find(host.helpHeard, HelpMessages::kFirstTurboName) !=
              host.helpHeard.end());
        CHECK(host.helpHeard == guest.helpHeard);
        REQUIRE(host.resultsCheckpoint);
        REQUIRE(guest.resultsCheckpoint);
        CHECK(host.resultsCheckpoint->kills >= host.player + 2);
        CHECK(host.resultsCheckpoint->gold == guest.resultsCheckpoint->gold);
        CHECK(host.resultsCheckpoint->experience == guest.resultsCheckpoint->experience);
        CHECK(host.resultsCheckpoint->kills == guest.resultsCheckpoint->kills);
        const auto first = CharacterProfile::capture(host.save);
        const auto second = CharacterProfile::capture(guest.save);
        REQUIRE(first);
        REQUIRE(second);
        CHECK(CharacterProfilePacket::encode(*first) == CharacterProfilePacket::encode(*second));
    }

    const auto shopEpoch = pair->hostSession.match().context().epoch;
    REQUIRE(pair->host.pause(kHostDevice));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Shopping);
    const auto localLane = [](const NativeOnlinePlay& play, s32 seat) -> const ShopLane& {
        const auto& lanes = play.shopScene()->session().lanes();
        const auto found =
            std::ranges::find(lanes, seat, [](const ShopLane& lane) { return lane.member.player; });
        REQUIRE(found != lanes.end());
        return *found;
    };
    REQUIRE(localLane(pair->host, kSeats[0]).phase == ShopPhase::Tally);
    // A guest held up while the host steps on catches up at its next poll, every step at once.
    for (u32 frame = 0; frame < 20; ++frame) {
        REQUIRE(pair->host.update({}) != NativeOnlinePlay::Phase::Failed);
    }
    REQUIRE(pair->guest.update({}) != NativeOnlinePlay::Phase::Failed);
    CHECK(pair->guestSession.match().shop().held() == 0);
    for (const s32 seat : kSeats) {
        CAPTURE(seat);
        CHECK(localLane(pair->guest, seat).phase == localLane(pair->host, seat).phase);
        CHECK(localLane(pair->guest, seat).cursor == localLane(pair->host, seat).cursor);
    }
    for (u32 frame = 0; frame < 90; ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].menu.select = second[kGuestDevice].menu.select = frame % 10 == 0;
        pair->step(first, second);
    }
    CHECK(localLane(pair->host, kSeats[0]).phase == ShopPhase::Tally);
    CHECK(localLane(pair->guest, kSeats[0]).phase == ShopPhase::Tally);
    CHECK(localLane(pair->host, kSeats[1]).phase != ShopPhase::Tally);
    REQUIRE(pair->host.resume(kHostDevice));
    for (u32 frame = 0; frame < 3600; ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        if (frame % 60 == 0) {
            first[kHostDevice].menu.start = second[kGuestDevice].menu.start = true;
        } else if (frame % 60 == 30) {
            first[kHostDevice].menu.select = second[kGuestDevice].menu.select = true;
        }
        pair->step(first, second);
        if (pair->host.phase() == NativeOnlinePlay::Phase::Playing &&
            pair->guest.phase() == NativeOnlinePlay::Phase::Playing) {
            break;
        }
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->host.scene());
    REQUIRE(pair->guest.scene());
    CHECK(pair->host.scene()->world()->isTower());
    CHECK(pair->guest.scene()->world()->isTower());
    // Every peer's tower is told the party has just beaten the fields.
    const std::optional<LevelBeaten> beaten = LevelBeaten{fields.realmId, fields.index};
    CHECK(pair->host.scene()->levelBeaten() == beaten);
    CHECK(pair->guest.scene()->levelBeaten() == beaten);
    CHECK(pair->hostSession.match().context() == pair->guestSession.match().context());
    CHECK(pair->hostSession.match().context().sourceEpoch == shopEpoch);
    CHECK(pair->hostSession.match().context().sourceTick == 0);
    CHECK(pair->hostStore.autoSaves == (autoSave ? 1U : 0U));
    CHECK(pair->guestStore.autoSaves == (autoSave ? 1U : 0U));
    REQUIRE(pair->host.hasCompletedProgress());
    REQUIRE(pair->guest.hasCompletedProgress());
    const auto canonical = pair->host.scene()->party();
    const auto agreed = *pair->hostSession.match().travelParty();
    const auto completedGold = pair->hostParty.localMembers()[0].save.gold;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<PlayerActor*>(pair->host.scene()->actor(kSeats[0]))->save().gold =
        completedGold + 777;
    REQUIRE(pair->host.saveCompletedProgress());
    CHECK(pair->hostParty.localMembers()[0].save.gold == completedGold);
    CHECK(pair->host.scene()->actor(kSeats[0])->save().gold == completedGold + 777);
    for (usize seat = 0; seat < agreed.size(); ++seat) {
        if (agreed[seat]) {
            CHECK(
                CharacterProfilePacket::encode(*agreed[seat]) ==
                CharacterProfilePacket::encode(*(*pair->hostSession.match().travelParty())[seat]));
        }
    }
    // Restore the test's deliberate live-only mutation before more shared ticks.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<PlayerActor*>(pair->host.scene()->actor(kSeats[0]))->save().gold =
        canonical[0].save.gold;
    pair->hostStore.saveSucceeds = pair->guestStore.saveSucceeds = false;
    CHECK_FALSE(pair->guest.saveCompletedProgress());
    CHECK(pair->guest.hasCompletedProgress());
    pair->hostStore.saveSucceeds = pair->guestStore.saveSucceeds = true;
    REQUIRE(pair->guest.saveCompletedProgress());
    CHECK(pair->hostStore.saves == 1);
    CHECK(pair->guestStore.saves == 2);
    CHECK(pair->hostParty.localMembers()[0].save.classUnlock == 0xFFFF);
    CHECK(pair->guestParty.localMembers()[0].save.classUnlock == 0xFFFF);
    for (const auto* play : {&pair->host, &pair->guest}) {
        for (const auto& member : play->scene()->participants()) {
            CHECK_FALSE(member.slot);
            CHECK_FALSE(member.resultsCheckpoint);
            CHECK(std::ranges::find(member.helpHeard, HelpMessages::kFirstTurboName) !=
                  member.helpHeard.end());
        }
    }
}

TEST_CASE("native secret travel resumes each retained parent and commits only local rewards",
          "[native-online-play][native-secret][assets]") {
    // B1 completion is part of the agreed initial profiles, not a fabricated
    // Prepare. It opens the actual Tower portal to B2, whose authored secret
    // exit reaches S4 through the same native contact phase on both peers.
    auto pair = std::make_unique<NativePair>(immediateTower(), "B1");
    pair->settle();
    pair->synchronize();
    const auto towerEpoch = pair->hostSession.match().context().epoch;
    for (u32 frame = 0; frame < 1800 && pair->hostSession.match().context().epoch == towerEpoch;
         ++frame) {
        pair->placeOnPortal(pair->host, "B2");
        pair->placeOnPortal(pair->guest, "B2");
        pair->step();
    }
    REQUIRE(pair->hostSession.match().context().transition == MatchTransition::Travel);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.scene());
    REQUIRE(pair->guest.scene());
    REQUIRE(pair->host.scene()->world()->ref().name == "B2");
    REQUIRE(pair->guest.scene()->world()->ref().name == "B2");
    pair->settle();
    pair->synchronize();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto* hostParent = const_cast<PlayScene*>(pair->host.scene());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto* guestParent = const_cast<PlayScene*>(pair->guest.scene());
    const auto* hostWorld = hostParent->world();
    const auto* guestWorld = guestParent->world();
    const auto parentEpoch = pair->hostSession.match().context().epoch;
    for (auto* scene : {hostParent, guestParent}) {
        for (const auto seat : kSeats) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            const_cast<PlayerActor*>(scene->actor(seat))->save().gold += 250 + seat;
            scene->awardExperience(seat, 100, true);
        }
    }
    const auto parentResults = hostParent->levelResults();
    for (u32 frame = 0; frame < 1800 && pair->hostSession.match().context().epoch == parentEpoch;
         ++frame) {
        pair->placeOnPortal(pair->host, "S4", true);
        pair->placeOnPortal(pair->guest, "S4", true);
        pair->step();
    }
    REQUIRE(pair->hostSession.match().context().transition == MatchTransition::SecretEnter);
    REQUIRE(pair->guestSession.match().context() == pair->hostSession.match().context());
    const auto hostFrozen = hostWorld->scene().geometry();
    const auto guestFrozen = guestWorld->scene().geometry();
    const auto consumed = [](const PlayScene& scene) {
        usize count = 0;
        for (usize index = 0; index < scene.portals().size(); ++index) {
            const auto& portal = scene.portals().portal(index);
            count += portal.secret && portal.consumed ? 1 : 0;
        }
        return count;
    };
    CHECK(consumed(*hostParent) == 1);
    CHECK(consumed(*guestParent) == 1);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.scene());
    REQUIRE(pair->guest.scene());
    REQUIRE(pair->host.scene()->world()->ref().name == "S4");
    REQUIRE(pair->guest.scene()->world()->ref().name == "S4");
    CHECK(pair->host.scene() != hostParent);
    CHECK(pair->guest.scene() != guestParent);
    pair->settle();
    pair->synchronize();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto* hostChallenge = const_cast<PlayScene*>(pair->host.scene());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto* guestChallenge = const_cast<PlayScene*>(pair->guest.scene());
    REQUIRE(hostChallenge->challenge().totalCoins() > 0);
    REQUIRE(hostChallenge->challenge().totalCoins() == guestChallenge->challenge().totalCoins());
    for (auto* scene : {hostChallenge, guestChallenge}) {
        for (const auto seat : kSeats) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            const_cast<PlayerActor*>(scene->actor(seat))->save().gold += 125;
            scene->awardExperience(seat, 80, true);
        }
    }
    const auto& items = hostChallenge->world()->placedItems();
    REQUIRE(items.size() == guestChallenge->world()->placedItems().size());
    for (usize index = 0; index < items.size(); ++index) {
        const auto& coin = items.item(index);
        if (coin.subtype != 1 || !coin.visible || coin.taken) {
            continue;
        }
        pair->synchronize();
        CHECK(coin.position == guestChallenge->world()->placedItems().item(index).position);
        for (auto* scene : {hostChallenge, guestChallenge}) {
            for (const auto seat : kSeats) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
                const_cast<PlayerActor*>(scene->actor(seat))->place(coin.position);
            }
        }
        pair->step();
        pair->step();
    }
    REQUIRE(hostChallenge->challenge().coinsLeft() == 0);
    REQUIRE(guestChallenge->challenge().coinsLeft() == 0);
    REQUIRE(hostChallenge->challenge().won());
    REQUIRE(guestChallenge->challenge().won());
    const auto reward = hostChallenge->challenge().rewardMask();
    REQUIRE(reward != 0);
    CHECK(guestChallenge->challenge().rewardMask() == reward);
    CHECK(pair->hostUnlocks.masks.empty());
    CHECK(pair->guestUnlocks.masks.empty());
    CHECK(pair->hostStore.autoSaves == 0);
    CHECK(pair->guestStore.autoSaves == 0);
    const auto challengeResults = hostChallenge->levelResults();
    const auto challengeEpoch = pair->hostSession.match().context().epoch;
    REQUIRE(hostChallenge->scroll().active());
    REQUIRE(guestChallenge->scroll().active());
    for (u32 frame = 0; frame < 1200 && pair->hostSession.match().context().epoch == challengeEpoch;
         ++frame) {
        // ALLCOINS is an actual native scroll: it holds the one-second won
        // countdown until both owned inputs accept and burn the message.
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].menu.select = second[kGuestDevice].menu.select = frame % 30 == 0;
        pair->step(first, second);
    }
    CHECK_FALSE(hostChallenge->scroll().active());
    CHECK_FALSE(guestChallenge->scroll().active());
    REQUIRE(pair->hostSession.match().context().transition == MatchTransition::SecretReturn);
    REQUIRE(pair->hostSession.match().context().challengeWon);
    const auto checkFrozen = [](const SceneGeometry& actual, const SceneGeometry& expected) {
        CHECK(actual.layout == expected.layout);
        CHECK(actual.objectCount == expected.objectCount);
        CHECK(actual.darken == expected.darken);
        REQUIRE(actual.objects.size() == expected.objects.size());
        for (usize index = 0; index < actual.objects.size(); ++index) {
            CAPTURE(index);
            CHECK(actual.objects[index].index == expected.objects[index].index);
            CHECK(actual.objects[index].continuity == expected.objects[index].continuity);
            CHECK(actual.objects[index].local == expected.objects[index].local);
            CHECK(actual.objects[index].alpha == expected.objects[index].alpha);
            CHECK(actual.objects[index].visible == expected.objects[index].visible);
        }
    };
    checkFrozen(hostWorld->scene().geometry(), hostFrozen);
    checkFrozen(guestWorld->scene().geometry(), guestFrozen);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->host.scene() == hostParent);
    CHECK(pair->guest.scene() == guestParent);
    CHECK(hostParent->world() == hostWorld);
    CHECK(guestParent->world() == guestWorld);
    CHECK_FALSE(hostParent->secretTravel());
    CHECK_FALSE(guestParent->secretTravel());
    CHECK(consumed(*hostParent) == 1);
    CHECK(consumed(*guestParent) == 1);
    CHECK(pair->hostUnlocks.masks == std::vector<u16>{reward});
    CHECK(pair->guestUnlocks.masks == std::vector<u16>{reward});
    CHECK(pair->hostStore.autoSaves == 1);
    CHECK(pair->guestStore.autoSaves == 1);
    CHECK(pair->hostParty.localMembers()[0].save.classUnlock == reward);
    CHECK(pair->guestParty.localMembers()[0].save.classUnlock == reward);
    REQUIRE(hostParent->levelResults().size() == kSeats.size());
    REQUIRE(guestParent->levelResults().size() == kSeats.size());
    for (usize index = 0; index < kSeats.size(); ++index) {
        const auto actual = hostParent->levelResults()[index];
        CHECK(actual.totals == guestParent->levelResults()[index].totals);
        CHECK(actual.totals == challengeResults[index].totals);
        CHECK(actual.totals[0] >= parentResults[index].totals[0] + 125);
        CHECK(actual.totals[2] > parentResults[index].totals[2]);
        CHECK_FALSE(hostParent->party()[index].slot);
        CHECK_FALSE(guestParent->party()[index].slot);
    }
    for (u32 frame = 0; frame < 120; ++frame) {
        pair->step();
    }
    CHECK(pair->hostUnlocks.masks.size() == 1);
    CHECK(pair->guestUnlocks.masks.size() == 1);
    CHECK(pair->hostStore.autoSaves == 1);
    CHECK(pair->guestStore.autoSaves == 1);
}
TEST_CASE("a parent resumed from a secret challenge rebuilds a guest divergence in place",
          "[native-online-play][native-secret][journal][assets]") {
    PlayOptions options;
    options.welcome = false;
    auto pair = std::make_unique<NativePair>(options, "B1", false, "B2");
    pair->settle();
    pair->synchronize();
    const auto arrive = [&](std::string_view name) {
        for (u32 frame = 0;
             frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                              pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
             ++frame) {
            pair->step();
        }
        REQUIRE(pair->host.scene());
        REQUIRE(pair->guest.scene());
        REQUIRE(pair->host.scene()->world()->ref().name == name);
        REQUIRE(pair->guest.scene()->world()->ref().name == name);
        pair->settle();
        pair->synchronize();
    };
    const auto parentEpoch = pair->hostSession.match().context().epoch;
    for (u32 frame = 0; frame < 1800 && pair->hostSession.match().context().epoch == parentEpoch;
         ++frame) {
        pair->placeOnPortal(pair->host, "S4", true);
        pair->placeOnPortal(pair->guest, "S4", true);
        pair->step();
    }
    REQUIRE(pair->hostSession.match().context().transition == MatchTransition::SecretEnter);
    arrive("S4");
    const auto& items = pair->host.scene()->world()->placedItems();
    for (usize index = 0; index < items.size(); ++index) {
        const auto& coin = items.item(index);
        if (coin.subtype != 1 || !coin.visible || coin.taken) {
            continue;
        }
        pair->synchronize();
        for (auto* play : {&pair->host, &pair->guest}) {
            for (const auto seat : kSeats) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
                const_cast<PlayerActor*>(play->scene()->actor(seat))->place(coin.position);
            }
        }
        pair->step();
        pair->step();
    }
    REQUIRE(pair->host.scene()->challenge().won());
    REQUIRE(pair->guest.scene()->challenge().won());
    const auto challengeEpoch = pair->hostSession.match().context().epoch;
    for (u32 frame = 0; frame < 1200 && pair->hostSession.match().context().epoch == challengeEpoch;
         ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].menu.select = second[kGuestDevice].menu.select = frame % 30 == 0;
        pair->step(first, second);
    }
    REQUIRE(pair->hostSession.match().context().transition == MatchTransition::SecretReturn);
    arrive("B2");
    const auto* resumed = pair->guest.scene();
    for (u32 frame = 0; frame < 40; ++frame) {
        pair->step();
    }
    pair->synchronize();
    // The guest's own resumed parent diverges, and no load can bring it back to its epoch.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): planted divergence
    ++const_cast<PlayerActor*>(pair->guest.scene()->actor(kSeats[1]))->save().gold;
    bool ordered = false;
    bool recovered = false;
    u64 orderedTick = 0;
    for (u32 frame = 0; frame < 600 && !recovered; ++frame) {
        pair->step();
        if (!ordered && pair->hostSession.match().resyncing()) {
            ordered = true;
            orderedTick = pair->hostSession.match().resync()->tick;
        }
        recovered = ordered && !pair->hostSession.match().resyncing() &&
                    !pair->guestSession.match().resyncing();
    }
    REQUIRE(ordered);
    REQUIRE(recovered);
    // Rebuilt in place: the same resumed scene goes on rather than a stage loaded afresh.
    CHECK(pair->guest.scene() == resumed);
    for (u32 frame = 0; frame < 150; ++frame) {
        pair->step();
    }
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    for (const auto seat : kSeats) {
        CHECK(pair->guest.scene()->actor(seat)->save().gold ==
              pair->host.scene()->actor(seat)->save().gold);
    }
    REQUIRE(pair->hostSession.match().nativeAgreement() != nullptr);
    CHECK(pair->hostSession.match().nativeAgreement()->agreedTick() > orderedTick);
    CHECK_FALSE(pair->hostSession.match().resyncing());
    CHECK_FALSE(pair->guestSession.match().resyncing());
}

TEST_CASE("native completion checkpoints save only local progress and honor Auto Save",
          "[native-online-play][native-save][native-completion][assets]") {
    (void)test::assetOrSkip("VQMOVIES/victory.avi");
    const bool autoSave = GENERATE(false, true);
    PlayOptions options;
    options.welcome = false;
    auto pair = std::make_unique<NativePair>(options, "", autoSave, "H4");
    pair->settle();
    pair->synchronize();
    for (const auto* play : {&pair->host, &pair->guest}) {
        REQUIRE(play->scene()->world()->level());
        REQUIRE(play->scene()->world()->level()->bossType == 44);
        // Prepare the same native wizard timeline immediately before its final
        // departure. The actual scene update must produce the ending boundary;
        // no Shop/ending packet or completion state is fabricated.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& victory = const_cast<BossVictory&>(play->scene()->victory());
        victory.begin(44, 'H', 0, 0, false);
        for (u32 step = 0; step < 20 && victory.stage() != BossVictory::Stage::Gone; ++step) {
            victory.update(600, {});
        }
        REQUIRE(victory.stage() == BossVictory::Stage::Gone);
    }
    for (u32 frame = 0; frame < 18000 && (pair->host.phase() != NativeOnlinePlay::Phase::Shopping ||
                                          pair->guest.phase() != NativeOnlinePlay::Phase::Shopping);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.shopScene());
    REQUIRE(pair->guest.shopScene());
    CHECK(pair->host.shopScene()->session().visit() == ShopVisit::Completion);
    CHECK(pair->guest.shopScene()->session().visit() == ShopVisit::Completion);
    REQUIRE(pair->hostSession.match().shop().setup());
    CHECK(pair->hostSession.match().shop().setup()->ending);
    CHECK(pair->hostStore.autoSaves == 0);
    CHECK(pair->guestStore.autoSaves == 0);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        first[kHostDevice].menu.select = second[kGuestDevice].menu.select = frame % 30 == 0;
        pair->step(first, second);
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->host.scene()->world()->isTower());
    CHECK(pair->guest.scene()->world()->isTower());
    CHECK(pair->hostStore.autoSaves == (autoSave ? 1U : 0U));
    CHECK(pair->guestStore.autoSaves == (autoSave ? 1U : 0U));
    CHECK(pair->host.hasCompletedProgress());
    CHECK(pair->guest.hasCompletedProgress());
    REQUIRE(pair->host.saveCompletedProgress());
    REQUIRE(pair->guest.saveCompletedProgress());
    CHECK(pair->hostStore.saves == 1);
    CHECK(pair->guestStore.saves == 1);
    for (const auto& member : pair->host.scene()->party()) {
        CHECK_FALSE(member.slot);
    }
}

TEST_CASE("native GameOver finishes locally while terminal input relay remains live",
          "[native-online-play][native-finished][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower());
    pair->settle();
    pair->synchronize();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<PlayScene*>(pair->host.scene())->startGameOver();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<PlayScene*>(pair->guest.scene())->startGameOver();
    for (u32 frame = 0; frame < static_cast<u32>(GameOver::kDurationTicks) + 30 &&
                        (pair->host.phase() != NativeOnlinePlay::Phase::Finished ||
                         pair->guest.phase() != NativeOnlinePlay::Phase::Finished);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Finished);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Finished);
    CHECK(pair->host.outcome() == NativeRollbackDriver::Status::GameOver);
    CHECK(pair->guest.outcome() == NativeRollbackDriver::Status::GameOver);
    CHECK_FALSE(pair->host.hasCompletedProgress());
    CHECK_FALSE(pair->host.pause(kHostDevice));
    const auto hostTick = pair->host.simulatedTick();
    const auto guestTick = pair->guest.simulatedTick();
    for (u32 frame = 0; frame < 10; ++frame) {
        pair->step();
    }
    CHECK(pair->host.simulatedTick() == hostTick);
    CHECK(pair->guest.simulatedTick() == guestTick);
    CHECK(pair->hostSession.phase() == OnlineSession::Phase::Active);
    CHECK(pair->guestSession.phase() == OnlineSession::Phase::Active);
}

TEST_CASE("native terminal departure waits for exact off-cadence state agreement",
          "[native-online-play][native-terminal-barrier][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower());
    pair->settle();
    pair->synchronize();
    // Leave enough distance from the periodic hash boundary to exercise the
    // independent exact-terminal report, not its periodic neighbour.
    while (pair->host.simulatedTick() % MatchSession::kNativeAgreementPeriod > 20) {
        pair->step();
    }
    pair->synchronize();
    const auto epoch = pair->hostSession.match().context().epoch;
    const auto* hostScene = pair->host.scene();
    const auto* guestScene = pair->guest.scene();
    for (const auto* play : {&pair->host, &pair->guest}) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& scene = *const_cast<PlayScene*>(play->scene());
        scene.startGameOver();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        const_cast<GameOver&>(scene.gameOver()).step(GameOver::kDurationTicks - 2);
    }
    usize blockedReports = 0;
    pair->guestWire.beforeSend = [&](std::span<const u8> bytes, PacketTransport::Delivery) {
        if (NativeAgreement::recognizesTerminal(bytes)) {
            ++blockedReports;
            return PacketTransport::SendResult::Congested;
        }
        return PacketTransport::SendResult::Sent;
    };
    for (u32 frame = 0; frame < 40; ++frame) {
        pair->step();
    }
    REQUIRE(blockedReports > 0);
    const auto tick = pair->host.confirmedTick();
    REQUIRE(tick > 0);
    REQUIRE(tick % MatchSession::kNativeAgreementPeriod != 0);
    CHECK(pair->guest.confirmedTick() == tick);
    CHECK(pair->host.simulatedTick() == tick);
    CHECK(pair->guest.simulatedTick() == tick);
    CHECK_FALSE(pair->hostSession.match().nativeTerminalAgreed(epoch, tick));
    CHECK_FALSE(pair->guestSession.match().nativeTerminalAgreed(epoch, tick));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->host.scene() == hostScene);
    CHECK(pair->guest.scene() == guestScene);
    CHECK(pair->hostStore.autoSaves == 0);
    CHECK(pair->guestStore.autoSaves == 0);
    CHECK(pair->hostUnlocks.masks.empty());
    CHECK(pair->guestUnlocks.masks.empty());

    pair->guestWire.beforeSend = {};
    for (u32 frame = 0; frame < 30 && (pair->host.phase() != NativeOnlinePlay::Phase::Finished ||
                                       pair->guest.phase() != NativeOnlinePlay::Phase::Finished);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Finished);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Finished);
    CHECK(pair->hostSession.match().nativeTerminalAgreed(epoch, tick));
    CHECK(pair->guestSession.match().nativeTerminalAgreed(epoch, tick));
    CHECK(pair->host.simulatedTick() == tick);
    CHECK(pair->guest.simulatedTick() == tick);
}

TEST_CASE("native online failures retain the session cause without saving speculative state",
          "[native-online-play][native-failure-detail][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower());
    pair->settle();
    pair->synchronize();
    REQUIRE(pair->host.failureDetail().empty());
    const auto completed = pair->host.simulatedTick();
    pair->hostService.closed = true;
    REQUIRE(pair->host.update({}) == NativeOnlinePlay::Phase::Failed);
    REQUIRE_FALSE(pair->hostSession.failureDetail().empty());
    CHECK(pair->host.failureDetail() == pair->hostSession.failureDetail());
    CHECK(pair->host.failureDetail() == "Room closed");
    const auto detail = std::string(pair->host.failureDetail());
    for (u32 tick = 0; tick < 20; ++tick) {
        CHECK(pair->host.update({}) == NativeOnlinePlay::Phase::Failed);
        CHECK(pair->host.failureDetail() == detail);
    }
    CHECK(pair->host.simulatedTick() == completed);
    CHECK(pair->hostStore.autoSaves == 0);
    CHECK(pair->hostUnlocks.masks.empty());
    pair->host.close();
    CHECK(pair->host.failureDetail().empty());
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Closed);
}
} // namespace
