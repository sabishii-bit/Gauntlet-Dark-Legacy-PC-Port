#include <catch2/catch_test_macros.hpp>

#include "game/netplay/OnlinePair.h"
#include "game/netplay/ShopSync.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = ShopSync::Admission;

u64 completeStartupFrame(test::OnlinePair& pair) {
    REQUIRE(pair.sample(pair.host, {}));
    REQUIRE(pair.sample(pair.guest, {}));
    pair.pump();
    const u64 tick = pair.hostInputs.tick();
    REQUIRE(pair.guestInputs.tick() == tick);
    REQUIRE(pair.advance(pair.host));
    REQUIRE(pair.advance(pair.guest));
    REQUIRE(pair.hostInputs.submitBoundaryDigest());
    REQUIRE(pair.guestInputs.submitBoundaryDigest());
    pair.pump();
    REQUIRE(pair.hostInputs.completeBoundary());
    REQUIRE(pair.guestInputs.completeBoundary());
    return tick;
}

struct Shops {
    ShopSync host;
    ShopSync guest;
    ShopSetup setup{1793, 3585, true, 0, {1000, 100, 1000}, {}};
    Shops() {
        host.begin(2, 1, {1, 2, 0, 0}, {11, 22, 0, 0}, &setup);
        guest.begin(2, 2, {1, 2, 0, 0}, {11, 22, 0, 0});
        relay(host, guest, 1);
    }
    static void relay(ShopSync& from, ShopSync& to, u8 peer) {
        while (!from.outgoing().empty()) {
            REQUIRE(to.receive(peer, from.outgoing().front().bytes) == Admission::Accepted);
            from.sent();
        }
    }
};
TEST_CASE("shop synchronization authenticates exact reliable menu edges before mutation",
          "[netplay][online-shop]") {
    Shops shops;
    REQUIRE(shops.guest.setup());
    CHECK(shops.guest.setup()->expanded);
    MenuInput buy;
    buy.select = true;
    CHECK_FALSE(shops.guest.sample(0, buy));
    REQUIRE(shops.guest.sample(1, buy, 4));
    const auto command = shops.guest.outgoing().front().bytes;
    auto spoofed = command;
    spoofed[16] = 0; // guest cannot operate the host's seat
    CHECK(shops.host.receive(2, spoofed) == Admission::WrongPeer);
    spoofed = command;
    spoofed[20] ^= 1; // correct owner, wrong epoch grant
    CHECK(shops.host.receive(2, spoofed) == Admission::WrongPeer);
    spoofed = command;
    spoofed[24] = 2; // a missing transaction is not silently skipped
    CHECK(shops.host.receive(2, spoofed) == Admission::Invalid);
    CHECK(shops.host.receive(2, command) == Admission::Accepted);
    CHECK(shops.host.receive(2, command) == Admission::Stale);
    auto step = shops.host.advance();
    REQUIRE(step);
    CHECK(step->inputs[1].select);
    CHECK(step->focus[1] == 4);
    REQUIRE(shops.host.publish(*step));
    Shops::relay(shops.host, shops.guest, 1);
    const auto shown = shops.guest.take();
    REQUIRE(shown);
    CHECK(shown->inputs[1].select);
    CHECK(shown->potions == step->potions);
    CHECK(shown->focus == step->focus);
    step = shops.host.advance();
    REQUIRE(step);
    CHECK_FALSE(step->inputs[1].select); // no repeated purchase from a held/replayed edge
    CHECK(step->focus[1] == -1);
    REQUIRE(shops.host.publish(*step));
    REQUIRE(shops.host.finish());
    CHECK_FALSE(shops.host.finished());
    REQUIRE(shops.guest.finish());
    shops.guest.sent(); // discard already admitted input
    Shops::relay(shops.guest, shops.host, 2);
    CHECK(shops.host.finished());
}
TEST_CASE("shop wire rejects malformed packets and bounds congestion", "[netplay][online-shop]") {
    Shops shops;
    MenuInput buy;
    buy.select = true;
    REQUIRE(shops.guest.sample(1, buy));
    auto bytes = shops.guest.outgoing().front().bytes;
    for (usize count = 0; count < bytes.size(); ++count) {
        CHECK(shops.host.receive(2, std::span{bytes}.first(count)) != Admission::Accepted);
    }
    bytes[33] = 0x80; // undefined action bits
    CHECK(shops.host.receive(2, bytes) == Admission::Invalid);
    bytes = shops.guest.outgoing().front().bytes;
    bytes[8] = 1;
    CHECK(shops.host.receive(2, bytes) == Admission::Stale);
    for (usize count = 0; count <= ShopSync::kBudget - 3; ++count) {
        const auto step = shops.host.advance();
        REQUIRE(step);
        REQUIRE(shops.host.publish(*step));
    }
    CHECK_FALSE(shops.host.advance());
    CHECK(shops.host.outgoing().size() <= ShopSync::kBudget);
}
TEST_CASE("the host folds a seat's waiting pointer moves into one, never its presses",
          "[netplay][online-shop]") {
    Shops shops;
    // A stall's worth of moves, a purchase, its release and a move after that, in one poll.
    for (s32 tick = 0; tick < 40; ++tick) {
        REQUIRE(shops.guest.sample(1, {}, tick % 8));
    }
    MenuInput buy;
    buy.select = true;
    REQUIRE(shops.guest.sample(1, buy, 5));
    REQUIRE(shops.guest.sample(1, {}));    // the release every press is followed by
    REQUIRE(shops.guest.sample(1, {}, 3)); // folds into the release, never into the press
    Shops::relay(shops.guest, shops.host, 2);
    std::vector<ShopStep> steps;
    for (s32 tick = 0; tick < 4; ++tick) {
        const auto step = shops.host.advance();
        REQUIRE(step);
        REQUIRE(shops.host.publish(*step));
        steps.push_back(*step);
    }
    CHECK(steps[0].focus[1] == 7); // the moves, folded into the latest
    CHECK_FALSE(steps[0].inputs[1].select);
    CHECK(steps[1].inputs[1].select); // the purchase, after them and once
    CHECK(steps[1].focus[1] == 5);
    CHECK_FALSE(steps[2].inputs[1].select);
    CHECK(steps[2].focus[1] == 3);
    CHECK(steps[3].focus[1] == -1);
}

TEST_CASE("the host's own presses wait and fold as a guest's do", "[netplay][online-shop]") {
    Shops shops;
    MenuInput buy;
    buy.select = true;
    REQUIRE(shops.host.sample(0, buy, 2));
    REQUIRE(shops.host.sample(0, {}));
    REQUIRE(shops.host.sample(0, {}, 4));
    std::vector<ShopStep> steps;
    for (s32 tick = 0; tick < 3; ++tick) {
        const auto step = shops.host.advance();
        REQUIRE(step);
        REQUIRE(shops.host.publish(*step));
        steps.push_back(*step);
    }
    CHECK(steps[0].inputs[0].select);
    CHECK(steps[0].focus[0] == 2);
    CHECK_FALSE(steps[1].inputs[0].select);
    CHECK(steps[1].focus[0] == 4);
    CHECK(steps[2].focus[0] == -1);
}

TEST_CASE("only presses count against what the host holds of a seat's",
          "[netplay][online-shop]") {
    Shops shops;
    MenuInput buy;
    buy.select = true;
    SECTION("a stall's clicks and their releases all reach the shop") {
        for (s32 click = 0; click < 40; ++click) {
            REQUIRE(shops.guest.sample(1, buy, 2));
            REQUIRE(shops.guest.sample(1, {}));
        }
        Shops::relay(shops.guest, shops.host, 2);
        s32 bought = 0;
        for (s32 tick = 0; tick < 80; ++tick) {
            const auto step = shops.host.advance();
            REQUIRE(step);
            REQUIRE(shops.host.publish(*step));
            bought += step->inputs[1].select ? 1 : 0;
        }
        CHECK(bought == 40);
    }
    SECTION("past ten seconds of presses a seat's next one is refused, their releases aside") {
        for (usize press = 0; press < ShopSync::kHeldPresses; ++press) {
            REQUIRE(shops.guest.sample(1, buy, 2));
            REQUIRE(shops.guest.sample(1, {})); // its release waits too, uncounted
            if (shops.guest.outgoing().size() >= 200) {
                Shops::relay(shops.guest, shops.host, 2);
            }
        }
        Shops::relay(shops.guest, shops.host, 2);
        REQUIRE(shops.guest.sample(1, buy, 2));
        CHECK(shops.host.receive(2, shops.guest.outgoing().front().bytes) == Admission::Invalid);
    }
}

TEST_CASE("a guest keeps every step the host sends while its own loop is held up",
          "[netplay][online-shop]") {
    Shops shops;
    for (usize tick = 0; tick < 300; ++tick) {
        const auto step = shops.host.advance();
        REQUIRE(step);
        REQUIRE(shops.host.publish(*step));
        if (tick % 50 == 49) {
            Shops::relay(shops.host, shops.guest, 1);
        }
    }
    usize shown = 0;
    while (shops.guest.take()) {
        ++shown;
    }
    CHECK(shown == 300);
}

TEST_CASE("shop colours remain authenticated host outcomes rather than guest RNG draws",
          "[netplay][online-shop][shop-random]") {
    Shops shops;
    for (u64 tick = 0; tick < 8; ++tick) {
        CHECK_FALSE(shops.guest.advance());
        auto step = shops.host.advance();
        REQUIRE(step);
        REQUIRE(step->tick == tick);
        for (const s32 colour : step->potions) {
            REQUIRE(colour >= 1);
            REQUIRE(colour <= 4);
        }
        // Ordered publication, not agreement between independently seeded engines,
        // decides the guest's colours, including idle and unowned seat slots.
        step->potions =
            tick % 2 == 0 ? std::array<s32, 4>{1, 2, 3, 4} : std::array<s32, 4>{4, 3, 2, 1};
        REQUIRE(shops.host.publish(*step));
        const auto packet = shops.host.outgoing().front().bytes;
        REQUIRE(shops.guest.receive(1, packet) == Admission::Accepted);
        CHECK(shops.guest.receive(1, packet) == Admission::Stale);
        shops.host.sent();
        const auto committed = shops.guest.take();
        REQUIRE(committed);
        CHECK(committed->tick == tick);
        CHECK(committed->potions == step->potions);
        CHECK_FALSE(shops.guest.take());
    }
}
TEST_CASE("shop setup authenticates the ending flag and rejects old or undefined wire flags",
          "[netplay][online-shop][online-completion]") {
    ShopSync host;
    ShopSync guest;
    ShopSetup setup{1793, 3585, true, 0, {1000, 100, 1000}, {}};
    setup.ending = true;
    host.begin(2, 1, {1, 2, 0, 0}, {11, 22, 0, 0}, &setup);
    guest.begin(2, 2, {1, 2, 0, 0}, {11, 22, 0, 0});
    REQUIRE(host.outgoing().size() == 1);
    auto packet = host.outgoing().front().bytes;
    CHECK(guest.receive(3, packet) == Admission::WrongPeer);
    packet[4] = 1;
    CHECK(guest.receive(1, packet) == Admission::Invalid);
    packet[4] = 2;
    packet[24] |= 4;
    CHECK(guest.receive(1, packet) == Admission::Invalid);
    CHECK_FALSE(guest.setup());
    Shops::relay(host, guest, 1);
    REQUIRE(guest.setup());
    CHECK(guest.setup()->expanded);
    CHECK(guest.setup()->ending);
}
TEST_CASE("a two-peer post-level epoch waits for profiles setup and every shop close",
          "[netplay][online-shop]") {
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    const u64 terminalTick = completeStartupFrame(pair);
    const auto party = *pair.hostSession.party();
    const ShopSetup setup{1, 3585, true, 0, {1000, 100, 1000}, {}};
    REQUIRE(pair.host.prepare(1, MatchTransition::Shop, &party, &setup));
    pair.pump();
    REQUIRE(pair.guest.completeNativeBoundary(pair.guest.context().epoch, terminalTick));
    REQUIRE(pair.host.travelParty());
    REQUIRE(pair.guest.travelParty());
    REQUIRE(pair.guest.shop().setup());
    CHECK(pair.guest.shop().setup()->expanded);
    pair.ready();
    CHECK_FALSE(pair.hostInputs.advance()); // combat cannot advance behind the shop
    MenuInput buy;
    buy.select = true;
    REQUIRE(pair.guest.shop().sample(1, buy));
    pair.pump();
    const auto step = pair.host.shop().advance();
    REQUIRE(step);
    CHECK(step->inputs[1].select);
    REQUIRE(pair.host.shop().publish(*step));
    pair.pump();
    REQUIRE(pair.guest.shop().take());
    REQUIRE(pair.host.shop().finish());
    CHECK_FALSE(pair.host.shop().finished());
    CHECK_FALSE(pair.host.prepare(3585, MatchTransition::Travel, &party));
    REQUIRE(pair.guest.shop().finish());
    pair.pump();
    CHECK(pair.host.shop().finished());
    REQUIRE(pair.host.prepare(3585, MatchTransition::Travel, &party));
    pair.pump();
    pair.ready();
    CHECK(pair.guest.context().scene == 3585);
    CHECK(pair.host.context() == pair.guest.context());
}
TEST_CASE("a dropped guest's pending shop transaction is discarded and the shop ends for the rest",
          "[netplay][online-shop][drop]") {
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    const u64 terminalTick = completeStartupFrame(pair);
    const auto party = *pair.hostSession.party();
    const ShopSetup setup{1, 3585, false, 0, {1000, 100, 1000}, {}};
    REQUIRE(pair.host.prepare(1, MatchTransition::Shop, &party, &setup));
    pair.pump();
    REQUIRE(pair.guest.completeNativeBoundary(pair.guest.context().epoch, terminalTick));
    pair.ready();
    MenuInput buy;
    buy.select = true;
    REQUIRE(pair.guest.shop().sample(1, buy));
    const auto pending = pair.guest.shop().outgoing().front().bytes;
    REQUIRE(pair.host.shop().finish());
    REQUIRE(pair.host.disconnected(1));
    CHECK(pair.host.phase() == MatchSession::Phase::Running);
    CHECK(pair.host.dropped(2));
    REQUIRE(pair.host.shop().setup());
    CHECK(pair.host.shop().finished()); // the dropped seat holds the shop no longer
    // What the dropped guest still sends is ignored: its purchase never reaches the shop.
    CHECK(pair.host.receive(1, pending) == MatchSession::Admission::Stale);
    const auto step = pair.host.shop().advance();
    REQUIRE(step);
    CHECK_FALSE(step->inputs[1].select);
}
} // namespace
