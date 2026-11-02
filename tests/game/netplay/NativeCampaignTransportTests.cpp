#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/net/SessionTransport.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/NativeOnlineIdentity.h"
#include "game/level/ExitPortals.h"
#include "game/netplay/HostedRoom.h"
#include "game/netplay/OnlineParty.h"
#include "game/netplay/OnlineSession.h"
#include "game/online/NativeOnlinePlay.h"
#include "game/scenes/AfterLevelScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Clock = std::chrono::steady_clock;

// These identities deliberately describe one in-process test contract. This
// exercises the real bootstrap exchange, not the separate file-hash audit.
const OnlineSession::NativeIdentity kIdentity{"campaign-transport-test", "same-process-runtime",
                                              std::string(64, 'a'), std::string(64, 'b'), "en"};

template <class Done, class Step>
void until(std::string_view stage, Done done, Step step,
           std::chrono::seconds limit = std::chrono::seconds(90)) {
    INFO("campaign transport stage: " << stage);
    const auto deadline = Clock::now() + limit;
    auto next = Clock::now();
    while (!done() && Clock::now() < deadline) {
        step();
        // Match/scene updates stay at the application's 60 Hz cadence. A
        // synchronous asset load consumes wall time, not fabricated input ticks
        // or a test-generated peer timeout; each phase has its own deadline.
        next = std::max(next + std::chrono::microseconds(16667), Clock::now());
        std::this_thread::sleep_until(next);
    }
    REQUIRE(done());
}

std::unique_ptr<SessionTransport> openTransport(const std::string& invitation = {}) {
    std::string error;
    auto wire = openSessionTransport({true, invitation}, error);
    REQUIRE(wire);
    until(
        "local endpoint ready", [&] { return wire->phase() != SessionTransport::Phase::Opening; },
        [] {}, std::chrono::seconds(15));
    // Invitations never enter assertion diagnostics or files.
    REQUIRE(wire->phase() == SessionTransport::Phase::Ready);
    return wire;
}

PlayOptions authoredExit(const std::filesystem::path& assets, const LevelCatalog& catalog,
                         const LevelRef& source) {
    test::FakeRenderDevice device;
    device.recordDraws = false;
    LevelWorld world;
    REQUIRE(world.load(device, assets, source));
    ExitPortals portals;
    REQUIRE(portals.bind(device, world.layout(), world.itemScope(), catalog, &world.collision()));
    for (usize index = 0; index < portals.size(); ++index) {
        const auto& portal = portals.portal(index);
        if (!portal.secret && !portal.shut && portal.destination && portal.minPlayers <= 2) {
            PlayOptions options;
            options.position = portal.position;
            options.yaw = 0;
            options.welcome = false;
            // The normal two-player spawn spreads each player one unit from
            // this centre. Reject an unsuitable fixture instead of teleporting
            // the actors after simulation/rollback has started.
            for (const f32 side : {-1.0f, 1.0f}) {
                const PortalVisitor visitor{portal.position + Vec3{side, 0, 0}, 0.75f};
                REQUIRE(portal.touches(visitor, 1));
            }
            return options;
        }
    }
    FAIL("G1 must contain an authored ordinary exit for the campaign test");
    return {};
}

struct CampaignPeer {
    std::unique_ptr<SessionTransport> wire;
    HostedRoom room;
    OnlineSession session;
    test::FakeRenderDevice device;
    GameConfig config;
    GameContext context;
    OnlineParty selection;
    std::unique_ptr<NativePlaySetup> setup;
    s32 localDevice;
    usize saveSlot;
    std::vector<PartyMember> saved;
    u32 saves = 0, unlocks = 0, manualSaves = 0;
    NativeOnlinePlay play; // Destroy borrowed scene/callback users before their owners.

    CampaignPeer(std::unique_ptr<SessionTransport> transport, bool host, s32 physicalDevice,
                 usize slot, const std::filesystem::path& assets, const LevelCatalog& levels,
                 const StringTable& strings)
        : wire(std::move(transport)), room(*wire, host, 1, kIdentity.build, kIdentity.content),
          session(room, room.transport(), host, 1), localDevice(physicalDevice), saveSlot(slot) {
        device.recordDraws = false;
        config.game.autoSave = true;
        context.config = &config;
        context.strings = &strings;
        context.levels = &levels;
        context.unpackedRoot = assets;
        context.saveLocalParty = [this](std::span<PartyMember> members) {
            REQUIRE(members.size() == 1);
            CHECK(members.front().player == localDevice);
            CHECK(members.front().slot == saveSlot);
            // The only allowed write in this campaign is the confirmed shop
            // result, not its preceding speculative source scene or tally.
            CHECK(session.match().context().transition == MatchTransition::Travel);
            CHECK(session.match().context().sourceTick == 0);
            CHECK(session.match().context().sourceEpoch == 2);
            saved.assign(members.begin(), members.end());
            ++saves;
        };
        context.manualSaveLocalParty = [this](std::span<PartyMember>) {
            ++manualSaves;
            return true;
        };
        context.unlockClasses = [this](u16) { ++unlocks; };
    }

    void confirmSetup(const PlayOptions& options) {
        if (!session.nativeSetupPending()) {
            return;
        }
        const auto* agreed = session.nativeContract();
        REQUIRE(agreed);
        REQUIRE(session.room());
        REQUIRE(session.party());
        const NativePlaySetup::Contract contract{
            agreed->rules.scene,        agreed->rules.autoMelee, agreed->rules.autoActivateItems,
            agreed->rules.expandedShop, agreed->rules.autoAim,   agreed->identity.build,
            agreed->identity.content,   agreed->identity.data};
        setup = NativePlaySetup::create(config, session.room()->settings, *session.party(), options,
                                        contract);
        REQUIRE(setup);
        REQUIRE(session.confirmNativeSetup(setup->digest()));
    }

    void checkHealthy() const {
        INFO("session failure: " << session.failureDetail());
        INFO("native scene failure: " << play.failureDetail());
        REQUIRE(session.phase() != OnlineSession::Phase::Failed);
        REQUIRE(play.phase() != NativeOnlinePlay::Phase::Failed);
    }

    void step(MenuInput menu = {}) {
        SessionInputs::Frame devices;
        auto& input = devices[static_cast<usize>(localDevice)];
        input.menu = std::move(menu);
        input.movieSkipPressed = play.entryPhase() == OnlineLevelEntry::Phase::Movie;
        play.update(devices);
        checkHealthy();
        if (play.entryVisible() || play.shopScene()) {
            play.render(device, Mat4{1}, 640, 448, 1.0 / 60, 1);
            checkHealthy();
        }
    }
};

void sameParty(std::span<const PartyMember> first, std::span<const PartyMember> second) {
    REQUIRE(first.size() == 2);
    REQUIRE(second.size() == first.size());
    for (const auto& member : first) {
        const auto remote = std::ranges::find(second, member.player, &PartyMember::player);
        REQUIRE(remote != second.end());
        CHECK_FALSE(member.slot);
        CHECK_FALSE(remote->slot);
        CHECK(member.fallen == remote->fallen);
        CHECK(member.helpHeard == remote->helpHeard);
        CHECK(member.resultsCheckpoint == remote->resultsCheckpoint);
        const auto left = CharacterProfile::capture(member.save);
        const auto right = CharacterProfile::capture(remote->save);
        REQUIRE(left);
        REQUIRE(right);
        CHECK(CharacterProfilePacket::encode(*left) == CharacterProfilePacket::encode(*right));
    }
}
} // namespace

TEST_CASE("real iroh campaign exits through tally and shop into the agreed tower",
          "[netplay][iroh][native-campaign-transport][assets]") {
    REQUIRE(NativeOnlineIdentity::prepareProcessNumeric());
    const auto assets = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    LevelCatalog levels;
    StringTable strings;
    REQUIRE(levels.load(assets));
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto source = levels.byName("G1");
    REQUIRE(source);
    const auto sourceScene = NativeOnlinePlay::sceneId(levels, *source);
    REQUIRE(sourceScene);
    const auto options = authoredExit(assets, levels, *source);
    // Load the fixture before opening either network endpoint. No loading cost
    // is misreported as missing network progress and no live state is patched.
    auto host =
        std::make_unique<CampaignPeer>(openTransport(), true, 2, 22, assets, levels, strings);
    auto guest = std::make_unique<CampaignPeer>(openTransport(host->wire->invitation()), false, 0,
                                                20, assets, levels, strings);
    const auto peers = std::array{host.get(), guest.get()};
    OnlineSession::NativeRules rules;
    rules.scene = *sourceScene;
    REQUIRE(host->session.configureNative(kIdentity, rules));
    REQUIRE(guest->session.configureNative(kIdentity));
    for (usize index = 0; index < peers.size(); ++index) {
        auto& peer = *peers[index];
        PartyMember member;
        member.player = peer.localDevice;
        member.slot = peer.saveSlot;
        member.save.name = index == 0 ? "HOST" : "GUEST";
        member.save.character = static_cast<s32>(index);
        member.save.color = static_cast<s32>(index);
        member.save.gold = 1000;
        member.save.progress().health = 1000;
        member.save.moviesSeen = {"LOCAL"};
        member.save.classes[index + 2].experience = 123456;
        REQUIRE(peer.selection.select(peer.session, std::array{member}));
    }
    const auto lobbyStep = [&] {
        for (auto* peer : peers) {
            peer->session.update(1.0 / 60);
            peer->confirmSetup(options);
            peer->checkHealthy();
        }
    };
    until(
        "room admission",
        [&] {
            return host->session.connected() && guest->session.connected() &&
                   (host->session.room() != nullptr) && host->session.room()->members.size() == 2;
        },
        lobbyStep);
    REQUIRE(host->session.ready());
    REQUIRE(guest->session.ready());
    until(
        "all room members ready",
        [&] {
            const auto* room = host->session.room();
            return (room != nullptr) && std::ranges::all_of(room->members, &RoomMember::ready);
        },
        lobbyStep);
    REQUIRE(host->session.start());
    until(
        "native setup agreement",
        [&] {
            return std::ranges::all_of(peers, [](const auto* peer) {
                return peer->session.phase() == OnlineSession::Phase::Active;
            });
        },
        lobbyStep);
    REQUIRE(host->setup);
    REQUIRE(guest->setup);
    CHECK(host->setup->digest() == guest->setup->digest());
    CHECK(host->session.match().context() == guest->session.match().context());
    // Bootstrap agrees the roster/setup, but no scene has been prepared yet.
    // NativeOnlinePlay::open on the host creates epoch 1; the guest adopts its
    // reliable Prepare while the ordinary loading loop pumps the connection.
    CHECK(host->session.match().context().epoch == 0);
    CHECK(host->session.match().phase() == MatchSession::Phase::Lobby);
    CHECK(guest->session.match().phase() == MatchSession::Phase::Lobby);
    for (auto* peer : peers) {
        REQUIRE(peer->play.open(peer->device, peer->context, peer->session, peer->selection,
                                std::move(peer->setup)));
    }
    CHECK(host->session.match().context().epoch == 1);
    CHECK(host->session.match().context().transition == MatchTransition::Start);
    CHECK(host->session.match().context().scene == *sourceScene);

    std::array<bool, 2> sawSource{};
    until(
        "authored exit and confirmed shop entry",
        [&] {
            return std::ranges::all_of(peers, [](const auto* peer) {
                return peer->play.phase() == NativeOnlinePlay::Phase::Shopping;
            });
        },
        [&] {
            for (usize index = 0; index < peers.size(); ++index) {
                auto& peer = *peers[index];
                peer.step();
                if (const auto* scene = peer.play.scene()) {
                    sawSource[index] |=
                        scene->world()->ref() == *source && peer.play.confirmedTick() > 0;
                }
                CHECK(peer.saves == 0);
                CHECK(peer.unlocks == 0);
            }
        });
    CHECK(std::ranges::all_of(sawSource, [](bool seen) { return seen; }));
    const auto shopContext = host->session.match().context();
    CHECK(shopContext == guest->session.match().context());
    REQUIRE(shopContext.epoch == 2);
    CHECK(shopContext.transition == MatchTransition::Shop);
    CHECK(shopContext.sourceEpoch == 1);
    CHECK(shopContext.sourceTick > 0);
    REQUIRE(host->play.shopScene());
    REQUIRE(guest->play.shopScene());
    sameParty(host->play.shopScene()->session().party(),
              guest->play.shopScene()->session().party());
    for (auto* peer : peers) {
        for (const auto& member : peer->play.shopScene()->session().party()) {
            CHECK(member.save.progress().levels.hasBeaten(source->realmId, source->index));
        }
    }

    u32 menuFrames = 0;
    std::array<bool, 2> sawShopping{};
    std::array<bool, 2> sawInventory{};
    until(
        "normal shop commands and tower return",
        [&] {
            return std::ranges::all_of(peers, [](const auto* peer) {
                return peer->play.phase() == NativeOnlinePlay::Phase::Playing &&
                       peer->play.scene() && peer->play.scene()->world()->isTower();
            });
        },
        [&] {
            for (usize index = 0; index < peers.size(); ++index) {
                auto& peer = *peers[index];
                if (const auto* shop = peer.play.shopScene()) {
                    REQUIRE(peer.session.seats().size() == 1);
                    const auto seat = peer.session.seats().front();
                    const auto& lanes = shop->session().lanes();
                    const auto own = std::ranges::find(
                        lanes, seat, [](const ShopLane& lane) { return lane.member.player; });
                    REQUIRE(own != lanes.end());
                    sawShopping[index] |= own->phase == ShopPhase::Shopping;
                    sawInventory[index] |= own->phase == ShopPhase::Inventory;
                }
                MenuInput menu;
                menu.start = menuFrames % 30 == 0;
                menu.select = menuFrames % 30 == 15;
                peer.step(menu);
            }
            ++menuFrames;
        });
    CHECK(std::ranges::all_of(sawShopping, [](bool seen) { return seen; }));
    CHECK(std::ranges::all_of(sawInventory, [](bool seen) { return seen; }));
    const auto towerContext = host->session.match().context();
    CHECK(towerContext == guest->session.match().context());
    CHECK(towerContext.epoch == shopContext.epoch + 1);
    CHECK(towerContext.transition == MatchTransition::Travel);
    CHECK(towerContext.sourceEpoch == shopContext.epoch);
    CHECK(towerContext.sourceTick == 0);
    sameParty(host->play.scene()->party(), guest->play.scene()->party());
    for (usize index = 0; index < peers.size(); ++index) {
        const auto& peer = *peers[index];
        REQUIRE(peer.saves == 1);
        REQUIRE(peer.saved.size() == 1);
        CHECK(peer.saved.front().save.name == (index == 0 ? "HOST" : "GUEST"));
        CHECK(peer.saved.front().save.classes[index + 2].experience == 123456);
        CHECK(peer.saved.front().save.moviesSeen == std::vector<std::string>{"LOCAL"});
        CHECK(peer.saved.front().save.progress().levels.hasBeaten(source->realmId, source->index));
        CHECK(peer.play.hasCompletedProgress());
    }
    const u64 confirmed = std::max(host->play.confirmedTick(), guest->play.confirmedTick());
    until(
        "new tower epoch remains live",
        [&] {
            return host->play.confirmedTick() >= confirmed + 90 &&
                   guest->play.confirmedTick() >= confirmed + 90;
        },
        [&] {
            for (auto* peer : peers) {
                peer->step();
                CHECK(peer->session.match().context() == towerContext);
                CHECK(peer->saves == 1);
                CHECK(peer->unlocks == 0);
                CHECK(peer->manualSaves == 0);
                CHECK(peer->play.simulatedTick() - peer->play.confirmedTick() <= 8);
            }
        });
}
