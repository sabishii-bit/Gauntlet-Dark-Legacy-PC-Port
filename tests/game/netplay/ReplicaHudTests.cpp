#include <bit>
#include <limits>
#include <ranges>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Bosses.h"
#include "game/netplay/CombatPlayback.h"
#include "game/screens/CinematicBars.h"
#include "game/screens/GameOver.h"
#include "game/screens/LevelArrivalPresentation.h"
#include "game/screens/LevelMessages.h"
#include "game/screens/ReplicaHud.h"
#include "game/screens/RuneMeter.h"
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    for (u32 i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}
HudPlayer player() {
    HudPlayer result;
    result.name = "KNIGHT";
    result.character = 5;
    result.color = 3;
    result.level = 99;
    result.health = 9999;
    result.gold = 99999;
    result.keys = result.potions = 9;
    result.potionKind = 4;
    result.runes = 0x1FFF;
    result.bossKeys = 0xFF;
    result.keysShown = true;
    result.turbo = HudTurbo{0.75f, {255, 0, 0, 255}, {255, 255, 0, 255}, 180, 4};
    result.usage = HudPowerup{12.34f, 9, 0, powerup::kPhoenix, true};
    result.selection = HudPowerup{-1, 5, 17, powerup::kSuperShot, false};
    result.labelY = StatusBoxPainter::kY - PowerupSelector::kLabelRise;
    return result;
}
TEST_CASE("HUD wire records are bounded canonical display values for each sparse seat roster",
          "[netplay][replica-hud]") {
    for (u32 mask = 0; mask < 16; ++mask) {
        HudSnapshot snapshot;
        snapshot.visible = (mask & 1U) != 0;
        for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
            if ((mask & (1U << seat)) != 0) {
                snapshot.players[seat] = player();
                snapshot.players[seat]->color = static_cast<s32>(seat);
            }
        }
        const auto bytes = HudPacket::encode(snapshot);
        REQUIRE(bytes);
        CHECK(bytes->size() == HudPacket::kHeaderBytes +
                                   static_cast<usize>(std::popcount(mask)) * HudPacket::kSeatBytes);
        const auto decoded = HudPacket::decode(*bytes);
        REQUIRE(decoded);
        CHECK(decoded->visible == snapshot.visible);
        CHECK(HudPacket::encode(*decoded) == bytes);
        for (usize size = 0; size < bytes->size(); ++size) {
            CHECK_FALSE(HudPacket::decode(std::span{*bytes}.first(size)));
        }
        auto extra = *bytes;
        extra.push_back(0);
        CHECK_FALSE(HudPacket::decode(extra));
    }
}
TEST_CASE("HUD rejects invalid flags names floats counts and hidden optional payloads",
          "[netplay][replica-hud]") {
    HudSnapshot snapshot;
    snapshot.players[0] = player();
    const auto bytes = HudPacket::encode(snapshot);
    REQUIRE(bytes);
    constexpr usize kSeat = HudPacket::kHeaderBytes;
    const std::array corruptions{
        std::pair{usize{0}, u32{0}},
        std::pair{usize{4}, u32{32}},
        std::pair{kSeat + 8, u32{17}},
        std::pair{kSeat + 12, u32{4}},
        std::pair{kSeat + 16, u32{0}},
        std::pair{kSeat + 16, u32{100}},
        std::pair{kSeat + 20, u32{0xFFFFFFFF}},
        std::pair{kSeat + 24, u32{1'000'001}},
        std::pair{kSeat + 28, u32{10}},
        std::pair{kSeat + 32, u32{10}},
        std::pair{kSeat + 36, u32{5}},
        std::pair{kSeat + 40, u32{65536}},
        std::pair{kSeat + 44, u32{65536}},
        std::pair{kSeat + 48, u32{64}},
        std::pair{kSeat + 52, std::bit_cast<u32>(1.1f)},
        std::pair{kSeat + 52, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN())},
        std::pair{kSeat + 64, u32{256}},
        std::pair{kSeat + 68, u32{5}},
        std::pair{kSeat + 72, u32{0}},
        std::pair{kSeat + 76, u32{4}},
        std::pair{kSeat + 80, std::bit_cast<u32>(std::numeric_limits<f32>::infinity())},
        std::pair{kSeat + 88, u32{2}},
        std::pair{kSeat + 108, u32{2}},
        std::pair{kSeat + 112, u32{513}}};
    for (const auto& [offset, value] : corruptions) {
        CAPTURE(offset, value);
        auto invalid = *bytes;
        word(invalid, offset, value);
        CHECK_FALSE(HudPacket::decode(invalid));
    }
    for (const auto [offset, value] :
         {std::pair{kSeat, u8{10}}, std::pair{kSeat + 6, u8{'X'}}, std::pair{kSeat + 7, u8{'X'}}}) {
        auto invalid = *bytes;
        invalid[offset] = value;
        CHECK_FALSE(HudPacket::decode(invalid));
    }
    snapshot.players[0] = HudPlayer{};
    const auto empty = HudPacket::encode(snapshot);
    REQUIRE(empty);
    for (usize offset = kSeat + 52; offset < empty->size(); ++offset) {
        auto invalid = *empty;
        invalid[offset] = 1;
        CHECK_FALSE(HudPacket::decode(invalid));
    }
    snapshot.players[0] = player();
    snapshot.players[0]->inTower = true;
    CHECK_FALSE(HudPacket::encode(snapshot));
}
TEST_CASE("combat checkpoints keep HUD updates atomic discrete and tied to occupied seats",
          "[netplay][replica-hud]") {
    CombatSnapshot first;
    first.motion.epoch = first.motion.cameraContinuity = 1;
    first.motion.players[2] = SeatMotion{1, 1, {}, 0};
    first.players[2] = PlayerCombatState{};
    first.hud = HudSnapshot{};
    first.hud->players[2] = player();
    first.hud->cards.push_back({2, HudCardKind::Key, 320});
    first.hud->counts[2] = HudCount{HudCountKind::Green, 3, 15};
    first.hud->bossBars.push_back({true, false, {256, 140}});
    first.hud->screen.transitionOpacity = 0.5f;
    first.hud->screen.titleScale = 1.5f;
    first.hud->hourglass = HudHourglass{0.25f, 2};
    first.hud->runeFill = 0.75f;
    first.hud->scroll = HudScroll{2, 1, -1, 128};
    first.hud->help = HudHelp{34, 2, 99, 5, false, Vec3{0, 2, 0}};
    FighterMeshState fighter;
    fighter.incarnation = fighter.part = fighter.resource = 1;
    first.fighters.push_back(fighter);
    const auto bytes = CombatPacket::encode(first);
    REQUIRE(bytes);
    const auto decoded = CombatPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(CombatPacket::encode(*decoded) == bytes);
    auto bad = *bytes;
    word(bad, 32, static_cast<u32>(HudPacket::kMaxBytes + 1));
    CHECK_FALSE(CombatPacket::decode(bad));
    bad = *bytes;
    bad.back() = 255;
    CHECK_FALSE(CombatPacket::decode(bad));
    auto invalid = first;
    invalid.hud->players[0] = player();
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = first;
    invalid.hud->players[2].reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    const auto send = [&](const CombatSnapshot& state) {
        const auto packets = CombatReplica::packets(state);
        REQUIRE(packets);
        for (const auto& packet : *packets) {
            const auto admission = playback.receive(1, packet);
            REQUIRE((admission == CombatReplica::Admission::Pending ||
                     admission == CombatReplica::Admission::Committed));
        }
    };
    send(first);
    auto next = first;
    next.motion.tick = 3;
    next.hud->visible = false;
    next.hud->screen = {false, true, 0, 0, 7};
    next.hud->hourglass.reset();
    next.hud->runeFill.reset();
    next.hud->scroll.reset();
    next.hud->help.reset();
    next.hud->players[2]->health = 10;
    next.hud->players[2]->usage.reset();
    next.hud->players[2]->selection->on = true;
    next.hud->cards.clear();
    next.hud->counts[2].reset();
    next.hud->bossBars[0] = {false, true, {40, 0}};
    send(next);
    const auto middle = playback.sample(1, 0.5f);
    REQUIRE(middle);
    CHECK(HudPacket::encode(*middle->hud) == HudPacket::encode(*first.hud));
    REQUIRE(playback.sample(3));
    CHECK(HudPacket::encode(*playback.sample(3)->hud) == HudPacket::encode(*next.hud));
}

TEST_CASE("screen overlays reject invalid ranges flags and contradictory display states",
          "[netplay][replica-hud]") {
    HudSnapshot state;
    state.screen = {false, false, 0.5f, 2, 0};
    const auto bytes = HudPacket::encode(state);
    REQUIRE(bytes);
    REQUIRE(HudPacket::decode(*bytes));
    CHECK(HudPacket::encode(*HudPacket::decode(*bytes)) == bytes);
    const std::array corruptions{
        std::pair{usize{20}, u32{4}},
        std::pair{usize{20}, u32{1}},
        std::pair{usize{20}, u32{2}},
        std::pair{usize{24}, std::bit_cast<u32>(-0.1f)},
        std::pair{usize{24}, std::bit_cast<u32>(1.1f)},
        std::pair{usize{24}, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN())},
        std::pair{usize{28}, std::bit_cast<u32>(-1.0f)},
        std::pair{usize{28}, std::bit_cast<u32>(2.1f)},
        std::pair{usize{28}, std::bit_cast<u32>(std::numeric_limits<f32>::infinity())},
        std::pair{usize{32}, u32{1}}};
    for (const auto& [offset, value] : corruptions) {
        auto bad = *bytes;
        word(bad, offset, value);
        CHECK_FALSE(HudPacket::decode(bad));
    }
    state.visible = false;
    state.screen = {true, false, 0.5f, 2, 0};
    REQUIRE(HudPacket::encode(state));
    state.screen = {false, true, 0, 0, 22};
    REQUIRE(HudPacket::encode(state));
    state.screen.gameOverLetters = 23;
    CHECK_FALSE(HudPacket::encode(state));
    state.screen.gameOverLetters = 0;
    state.screen.cinematicBars = true;
    CHECK_FALSE(HudPacket::encode(state));
    state.screen.cinematicBars = false;
    state.screen.titleScale = 1;
    CHECK_FALSE(HudPacket::encode(state));
    state.screen.titleScale = 0;
    state.screen.transitionOpacity = 0.5f;
    CHECK_FALSE(HudPacket::encode(state));
}

TEST_CASE("HUD timer records reject malformed and hidden optional data", "[netplay][replica-hud]") {
    HudSnapshot state;
    state.hourglass = HudHourglass{0.125f, -1};
    state.runeFill = 1.0f;
    const auto bytes = HudPacket::encode(state);
    REQUIRE(bytes);
    REQUIRE(HudPacket::decode(*bytes));
    CHECK(HudPacket::encode(*HudPacket::decode(*bytes)) == bytes);
    for (const auto [offset, value] :
         {std::pair{usize{36}, u32{4}}, std::pair{usize{36}, u32{0}}, std::pair{usize{36}, u32{1}},
          std::pair{usize{36}, u32{2}}, std::pair{usize{40}, std::bit_cast<u32>(-0.01f)},
          std::pair{usize{40}, std::bit_cast<u32>(1.01f)},
          std::pair{usize{40}, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN())},
          std::pair{usize{44}, std::bit_cast<u32>(-0.01f)},
          std::pair{usize{44}, std::bit_cast<u32>(1.01f)},
          std::pair{usize{44}, std::bit_cast<u32>(std::numeric_limits<f32>::infinity())},
          std::pair{usize{48}, u32{0xFFFFFFFE}}, std::pair{usize{48}, u32{4096}}}) {
        auto bad = *bytes;
        word(bad, offset, value);
        CHECK_FALSE(HudPacket::decode(bad));
    }
    const auto empty = HudPacket::encode(HudSnapshot{});
    REQUIRE(empty);
    for (usize offset = 40; offset < 52; ++offset) {
        auto bad = *empty;
        bad[offset] = 1;
        CHECK_FALSE(HudPacket::decode(bad));
    }
    state.hourglass->fallingFrame = 4095;
    REQUIRE(HudPacket::encode(state)); // Native resource admission imposes the tighter bound.
    state.hourglass->elapsed = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(HudPacket::encode(state));
    state.hourglass.reset();
    state.runeFill = -1.0f;
    CHECK_FALSE(HudPacket::encode(state));
}

TEST_CASE("pickup and boss overlays have fixed bounded wire identities and exact extents",
          "[netplay][replica-hud]") {
    HudSnapshot state;
    for (auto& slot : state.players) {
        slot = player();
    }
    for (usize i = 0; i < HudSnapshot::kMaxCards; ++i) {
        state.cards.push_back({static_cast<u32>(i % InputCommand::kSeats),
                               static_cast<HudCardKind>(i % static_cast<usize>(HudCardKind::Count)),
                               304 + static_cast<s32>(i)});
    }
    for (auto& count : state.counts) {
        count = HudCount{HudCountKind::Sumner, -1, 250};
    }
    state.bossBars.resize(HudSnapshot::kMaxBossBars, {true, true, {256, 0}});
    const auto bytes = HudPacket::encode(state);
    REQUIRE(bytes);
    CHECK(bytes->size() == HudPacket::kMaxBytes);
    const auto decoded = HudPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(HudPacket::encode(*decoded) == bytes);
    for (usize i = 0; i < bytes->size(); ++i) {
        CHECK_FALSE(HudPacket::decode(std::span{*bytes}.first(i)));
    }
    constexpr usize kCard = HudPacket::kHeaderBytes + InputCommand::kSeats * HudPacket::kSeatBytes;
    constexpr usize kCount = kCard + HudSnapshot::kMaxCards * HudPacket::kCardBytes;
    constexpr usize kBar = kCount + InputCommand::kSeats * HudPacket::kCountBytes;
    const std::array corruptions{std::pair{usize{8}, u32{25}},
                                 std::pair{usize{8}, u32{0xFFFFFFFF}},
                                 std::pair{usize{12}, u32{16}},
                                 std::pair{usize{16}, u32{4}},
                                 std::pair{kCard, u32{4}},
                                 std::pair{kCard + 4, static_cast<u32>(HudCardKind::Count)},
                                 std::pair{kCard + 4, u32{256}},
                                 std::pair{kCard + 8, u32{303}},
                                 std::pair{kCard + 8, u32{400}},
                                 std::pair{kCount, static_cast<u32>(HudCountKind::Count)},
                                 std::pair{kCount, u32{256}},
                                 std::pair{kCount + 4, u32{1'000'001}},
                                 std::pair{kCount + 8, u32{0xFFFFFFFF}},
                                 std::pair{kBar, u32{4}},
                                 std::pair{kBar + 4, u32{257}},
                                 std::pair{kBar + 8, u32{0xFFFFFFFF}}};
    for (const auto& [offset, value] : corruptions) {
        CAPTURE(offset, value);
        auto bad = *bytes;
        word(bad, offset, value);
        CHECK_FALSE(HudPacket::decode(bad));
    }
    state.cards.push_back({});
    CHECK_FALSE(HudPacket::encode(state));
    state.cards.pop_back();
    state.bossBars.push_back({});
    CHECK_FALSE(HudPacket::encode(state));
    CHECK(ReplicaHud::cardTexture(HudCardKind::Count).empty());
    CHECK(ReplicaHud::countTexture(HudCountKind::Count).empty());
}

TEST_CASE("scroll and help wire fields reject unknown flags ranges and noncanonical padding",
          "[netplay][replica-hud]") {
    HudSnapshot state;
    state.scroll = HudScroll{2, 1, -1, 128};
    state.help = HudHelp{34, 3, 99, 5, true, Vec3{12, 34, -56}};
    const auto bytes = HudPacket::encode(state);
    REQUIRE(bytes);
    const auto decoded = HudPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(HudPacket::encode(*decoded) == bytes);
    const std::array corruptions{
        std::pair{usize{52}, u32{4}},
        std::pair{usize{56}, u32{4096}},
        std::pair{usize{60}, u32{256}},
        std::pair{usize{64}, u32{21}},
        std::pair{usize{64}, u32{0xFFFFFFFE}},
        std::pair{usize{68}, u32{256}},
        std::pair{usize{72}, u32{151}},
        std::pair{usize{76}, u32{4}},
        std::pair{usize{76}, u32{0xFFFFFFFE}},
        std::pair{usize{80}, u32{1'000'001}},
        std::pair{usize{80}, u32{0xFFFFFFFE}},
        std::pair{usize{84}, u32{17}},
        std::pair{usize{84}, u32{0xFFFFFFFE}},
        std::pair{usize{88}, u32{4}},
        std::pair{usize{92}, std::bit_cast<u32>(std::numeric_limits<f32>::infinity())},
        std::pair{usize{96}, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN())},
        std::pair{usize{100}, std::bit_cast<u32>(-1'000'001.0f)}};
    for (const auto& [offset, value] : corruptions) {
        CAPTURE(offset, value);
        auto bad = *bytes;
        word(bad, offset, value);
        CHECK_FALSE(HudPacket::decode(bad));
    }
    state.help->anchor.reset();
    auto noAnchor = HudPacket::encode(state);
    REQUIRE(noAnchor);
    for (usize i = 92; i < 104; ++i) {
        auto bad = *noAnchor;
        bad[i] = 1;
        CHECK_FALSE(HudPacket::decode(bad));
    }
    state.scroll.reset();
    state.help.reset();
    const auto absent = HudPacket::encode(state);
    REQUIRE(absent);
    for (usize i = 56; i < 104; ++i) {
        auto bad = *absent;
        bad[i] = 1;
        CHECK_FALSE(HudPacket::decode(bad));
    }
}

void compare(std::span<const test::RecordedDraw> actual,
             std::span<const test::RecordedDraw> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize i = 0; i < actual.size(); ++i) {
        CAPTURE(i);
        const auto& a = actual[i];
        const auto& b = expected[i];
        REQUIRE(a.vertices.size() == b.vertices.size());
        REQUIRE(a.texture != nullptr);
        REQUIRE(b.texture != nullptr);
        CHECK(a.texture->width() == b.texture->width());
        CHECK(a.texture->height() == b.texture->height());
        const auto* actualTexture = dynamic_cast<const test::FakeTexture*>(a.texture);
        const auto* expectedTexture = dynamic_cast<const test::FakeTexture*>(b.texture);
        REQUIRE(actualTexture != nullptr);
        REQUIRE(expectedTexture != nullptr);
        CHECK(actualTexture->pixels == expectedTexture->pixels);
        CHECK(a.state.depthWrite == b.state.depthWrite);
        CHECK(a.state.depthTest == b.state.depthTest);
        CHECK(a.blend() == b.blend());
        CHECK(a.transform == b.transform);
        for (usize n = 0; n < a.vertices.size(); ++n) {
            CHECK(a.vertices[n].position == b.vertices[n].position);
            CHECK(a.vertices[n].uv == b.vertices[n].uv);
            CHECK(a.vertices[n].color == b.vertices[n].color);
        }
    }
}
TEST_CASE("replicated scroll pages pulse and burn exactly like native host artwork without uploads",
          "[netplay][replica-hud][assets]") {
    const auto root = test::assetOrSkip("STATIC/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    TextureSet textures;
    LevelMessages messages;
    messages.load(device, textures, root, &strings);
    PartyHud host;
    REQUIRE(host.load(device, root, &strings));
    ReplicaHud client;
    REQUIRE(client.load(device, root, &strings));
    MessageTable table;
    REQUIRE(table.load(root / "text/scroll_e.json"));
    const auto welcome = table.find("WELCOMEMESSAGE");
    REQUIRE(welcome);
    Canvas canvas;
    const Mat4 projection = makeLetterboxProjection(512, 384, 1920, 1080);
    HudSnapshot state;
    const auto check = [&] {
        const auto look = messages.look();
        state.scroll.reset();
        if (look) {
            state.scroll = HudScroll{look->message, look->page, look->burnFrame, look->promptAlpha};
        }
        const auto bytes = HudPacket::encode(state);
        REQUIRE(bytes);
        const auto received = HudPacket::decode(*bytes);
        REQUIRE(received);
        REQUIRE(client.accepts(*received));
        messages.prepare(device);
        device.draws.clear();
        canvas.begin(device, projection);
        if (state.visible) {
            host.drawStatus(canvas, {});
        }
        if (state.screen.cinematicBars) {
            drawCinematicBars(canvas, 384);
        }
        messages.draw(canvas);
        canvas.end();
        const auto expected = device.draws;
        const auto allocations = device.texturesCreated;
        const auto uploads = device.textureUpdates;
        for (s32 repeat = 0; repeat < 3; ++repeat) {
            device.draws.clear();
            canvas.begin(device, projection);
            client.draw(canvas, *received);
            canvas.end();
            compare(device.draws, expected);
        }
        CHECK(device.texturesCreated == allocations);
        CHECK(device.textureUpdates == uploads);
        CHECK(HudPacket::encode(*received) == bytes);
    };
    REQUIRE(messages.open(device, "WELCOMEMESSAGE", &strings));
    const auto pages = messages.scroll().pageCount();
    for (usize page = 0; page < pages; ++page) {
        REQUIRE(messages.look());
        CHECK(messages.look()->page == page);
        for (s32 tick = 0; tick < 45; ++tick) {
            check();
            messages.step(1, 0);
        }
        messages.step(1, 1);
    }
    REQUIRE(messages.scroll().burning());
    for (s32 frame = 0; frame < BurnDialogueScroll::kFrameCount; ++frame) {
        REQUIRE(messages.look());
        CHECK(messages.look()->burnFrame == frame);
        check();
        messages.step(BurnDialogueScroll::kTicksPerFrame, 0);
    }
    CHECK_FALSE(messages.look());
    check();
    // Opening just a page must retain its absolute native index, even over cinema bars.
    REQUIRE(pages > 1);
    REQUIRE(messages.open(device, "WELCOMEMESSAGE", &strings, pages - 1));
    REQUIRE(messages.look());
    CHECK(messages.look()->page == pages - 1);
    state.visible = false;
    state.screen.cinematicBars = true;
    check();
    messages.step(ScrollBox::kHoldTicks, 0);
    messages.step(1, 1);
    messages.step(18, 0); // A client may first receive a late burn frame.
    check();
    state.scroll = HudScroll{4095, 0, -1, 0};
    REQUIRE(state.valid());
    CHECK_FALSE(client.accepts(state));
    state.scroll = HudScroll{*welcome, 255, -1, 0};
    CHECK_FALSE(client.accepts(state));
    client.clear();
    CHECK_FALSE(client.accepts(state));
}

TEST_CASE("replicated help resolves all native messages without marking them seen or ticking",
          "[netplay][replica-hud][assets]") {
    const auto root = test::assetOrSkip("STATIC/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    PartyHud host;
    REQUIRE(host.load(device, root, &strings));
    ReplicaHud client;
    REQUIRE(client.load(device, root, &strings));
    std::array<PlayerRuntime, 2> players;
    TextureSet textures;
    REQUIRE(textures.load(root / "STATIC"));
    players[0].actor.spawn(3, {}, nullptr, Vec3{20, 30, 40}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{-20, 10, -15},
                           0); // Behind the camera: use screen center.
    const Mat4 projection = makeLetterboxProjection(512, 384, 1920, 1080);
    const Mat4 worldToCanvas =
        WorldCamera::frameMapping(512, 384) * WorldCamera::projection(1.2f, 16.0f / 9.0f);
    Canvas canvas;
    usize checked = 0;
    for (s32 id = 0; id <= 150; ++id) {
        if (HelpMessages::specOf(id) == nullptr) {
            HudSnapshot bad;
            bad.help = HudHelp{};
            bad.help->id = id;
            REQUIRE(bad.valid());
            CHECK_FALSE(client.accepts(bad));
            continue;
        }
        for (const s32 seat : {-1, 1, 3}) {
            CAPTURE(id, seat);
            host.help().clear();
            std::vector<s32> seen;
            std::vector<s32> heard;
            const std::array readers{HelpReader{-1, &seen, &heard}, HelpReader{1, &seen, &heard},
                                     HelpReader{3, &seen, &heard}};
            REQUIRE(host.help().post(id, seat, readers, 99, {5, seat == 3}));
            ++checked;
            const auto savedSeen = seen;
            const auto savedHeard = heard;
            for (const s32 ticks : {0, 25, 1000}) {
                host.help().update(ticks);
                const auto state = HudCapture::capture(players, host, true);
                REQUIRE(state);
                CHECK(state->help.has_value() == host.help().showing());
                const auto bytes = HudPacket::encode(*state);
                REQUIRE(bytes);
                const auto received = HudPacket::decode(*bytes);
                REQUIRE(received);
                REQUIRE(client.accepts(*received));
                device.draws.clear();
                canvas.begin(device, projection);
                host.drawStatus(canvas, players);
                host.drawHelp(canvas, device, textures, players, projection * worldToCanvas,
                              projection, 512, 384);
                canvas.end();
                const auto expected = device.draws;
                const auto allocations = device.texturesCreated;
                const auto uploads = device.textureUpdates;
                for (s32 repeat = 0; repeat < 2; ++repeat) {
                    device.draws.clear();
                    canvas.begin(device, projection);
                    client.draw(canvas, *received, worldToCanvas);
                    canvas.end();
                    compare(device.draws, expected);
                }
                CHECK(device.texturesCreated == allocations);
                CHECK(device.textureUpdates == uploads);
                CHECK(seen == savedSeen);
                CHECK(heard == savedHeard);
            }
        }
    }
    CHECK(checked >= 300);
}

TEST_CASE("replicated hourglass and rune meter use host samples without client clocks",
          "[netplay][replica-hud][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/textures.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    ChallengeHud challenge;
    REQUIRE(challenge.bind(device, powerups));
    PartyHud host;
    REQUIRE(host.load(device, root, nullptr));
    REQUIRE(host.bindHourglass(device, powerups));
    TextureSet textures;
    REQUIRE(textures.load(root / "STATIC"));
    const auto frame = textures.find("THERMBASE");
    const auto column = textures.find("THERMCOL");
    REQUIRE(frame);
    REQUIRE(column);
    const auto& runeFrame = textures.texture(device, *frame);
    const auto& runeColumn = textures.texture(device, *column);
    std::array<PartyMember, 1> party{};
    party[0].save.progress().relics.addShard(9);
    RuneMeter rune;
    rune.begin(0, Vec3{0}, Vec3{100, 0, 0}, 7, party);
    REQUIRE(rune.visible());
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0}, 0);
    auto& third = players[0].actor.save().progress().inventory;
    auto& first = players[1].actor.save().progress().inventory;
    third.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 2);
    first.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 1);
    host.focusPickup(players[1].actor, powerup::kSpecial, powerup::kStopTime);
    ReplicaHud client;
    REQUIRE(client.load(device, root, nullptr));
    HudSnapshot needsTimer;
    needsTimer.hourglass = HudHourglass{0, 0};
    CHECK_FALSE(client.accepts(needsTimer));
    HudResources resources;
    resources.powerups = &powerups;
    REQUIRE(client.load(device, root, nullptr, resources));
    CHECK(client.accepts(needsTimer));
    needsTimer.hourglass->fallingFrame = 4095;
    REQUIRE(needsTimer.valid());
    CHECK_FALSE(client.accepts(needsTimer));
    Canvas canvas;
    const Mat4 projection = makeLetterboxProjection(512, 384, 1920, 1080);
    for (s32 tick = 0; tick <= 150; ++tick) {
        CAPTURE(tick);
        if (tick == 10) {
            first.powerups[0].on = false;
        }
        if (tick == 20) {
            first.powerups[0].on = true;
        }
        if (tick == 30) {
            players[1].life = PlayerLife::Dying;
        }
        if (tick == 40) {
            players[0].departed = true;
        }
        if (tick == 50) {
            players[0].departed = false;
        }
        rune.update(Vec3{100 - static_cast<f32>(tick), 0, 0}, tick < 100);
        const bool running = tick < 80 || tick >= 90;
        const auto look = host.hourglassLook(players).value_or(
            challenge.look(150 - static_cast<f32>(tick), 150, running));
        auto snapshot = HudCapture::capture(players, host, true);
        REQUIRE(snapshot);
        snapshot->hourglass = HudHourglass{look.elapsed, look.fallingFrame};
        if (rune.visible()) {
            snapshot->runeFill = rune.fill();
        }
        const auto bytes = HudPacket::encode(*snapshot);
        REQUIRE(bytes);
        const auto received = HudPacket::decode(*bytes);
        REQUIRE(received);
        REQUIRE(client.accepts(*received));
        for (s32 repeat = 0; repeat < 3; ++repeat) {
            const auto allocations = device.texturesCreated;
            device.draws.clear();
            canvas.begin(device, projection);
            client.draw(canvas, *received);
            canvas.end();
            const auto actual = device.draws;
            CHECK(device.texturesCreated == allocations);
            device.draws.clear();
            canvas.begin(device, projection);
            host.drawStatus(canvas, players);
            rune.draw(canvas, runeFrame, runeColumn);
            if (!host.drawHourglass(canvas, players)) {
                challenge.draw(canvas, 150 - static_cast<f32>(tick), 150, running);
            }
            canvas.end();
            compare(actual, device.draws);
            CHECK(HudPacket::encode(*received) == bytes);
        }
        first.advance(1.0f / 60);
        third.advance(1.0f / 60);
        host.stepHourglass(1.0f / 60, players);
        if (running) {
            challenge.step(1.0f / 30);
        }
    }
    client.clear();
    REQUIRE(client.load(device, root, nullptr));
    needsTimer.hourglass->fallingFrame = -1;
    CHECK_FALSE(client.accepts(needsTimer)); // A new scene cannot retain borrowed timer art.
}

TEST_CASE("replicated HUD renders the native cards and selectors without ticking or saving",
          "[netplay][replica-hud][assets]") {
    const auto root = test::assetOrSkip("STATIC/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    TextureSet textures;
    LevelMessages messages;
    messages.load(device, textures, root, &strings);
    REQUIRE(messages.text().ready());
    const auto glow = textures.find("FONT32_GLOW");
    REQUIRE(glow);
    PartyHud host;
    REQUIRE(host.load(device, root, &strings));
    host.setGlow(&textures.texture(device, *glow));
    host.showRelics();
    ReplicaHud client;
    REQUIRE(client.load(device, root, &strings));
    std::array<PlayerRuntime, 4> players;
    LevelSoundscape audio;
    for (usize index = 0; index < players.size(); ++index) {
        CharacterSave save;
        save.name = "TEST";
        save.character = static_cast<s32>(index);
        save.color = static_cast<s32>(3 - index);
        save.gold = 57;
        save.progress().experience = levelExperience(80);
        save.progress().health = 1234;
        save.progress().relics.runes = 0x1FFF;
        save.progress().relics.shards = 0xFF;
        save.progress().inventory.addKeys(9);
        save.progress().inventory.addPotions(static_cast<s32>(index + 1), 9);
        save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 12.34f);
        save.progress().inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 17, -1);
        auto& runtime = players[index];
        runtime.actor.spawn(static_cast<s32>(3 - index), save, nullptr, {}, 0);
        runtime.turbo.add(static_cast<f32>(index) * 35);
        runtime.turbo.step(100);
        host.focusPickup(runtime.actor, powerup::kSpecial, powerup::kPhoenix);
        host.stepSelector(runtime.actor, {.up = true}, 32, audio);
        host.stepSelector(runtime.actor, {}, 1, audio);
    }
    for (s32 tick = 0; tick < 360; ++tick) {
        CAPTURE(tick);
        if (tick == 0 || tick == 60) {
            host.pickups().addCard(0, "KEY");
            host.pickups().addCard(3, "FRUIT");
            host.pickups().addCard(0, "MAGIC"); // Later card draws above the earlier one.
            host.pickups().showCount(0, PickupHud::crystalIcon(1), 3 + tick, 250);
            host.pickups().showCount(3, "SM_FANGS", 7, 12);
        }
        if (tick == 10) {
            host.stepSelector(players[0].actor, {.up = true}, 1, audio);
        }
        if (tick == 20) {
            host.stepSelector(players[0].actor, {.right = true}, 1, audio);
        }
        if (tick == 30) {
            players[1].life = PlayerLife::Dying;
        }
        if (tick == 40) {
            players[1].life = PlayerLife::InTower;
            players[1].towerPrompt = true;
            host.stepSelector(players[1].actor, {.down = true}, 32, audio);
        }
        if (tick == 50) {
            players[2].departed = true;
            host.stepSelector(players[2].actor, {.down = true}, 32, audio);
        }
        for (auto& runtime : players) {
            runtime.actor.save().progress().inventory.advance(1.0f / 60);
            runtime.turbo.step(1);
            host.stepSelector(runtime.actor, {}, 1, audio);
        }
        host.stepRelics(5);
        host.pickups().step(1, 1.0f / 60);
        if (tick == 26) {
            // A final charged shot can empty a slot after this tick's selector update.
            players[0].actor.save().progress().inventory.powerups[1].strength = 0;
        }
        const auto captured = HudCapture::capture(players, host, true);
        REQUIRE(captured);
        const auto bytes = HudPacket::encode(*captured);
        REQUIRE(bytes);
        const auto received = HudPacket::decode(*bytes);
        REQUIRE(received);
        const auto save = players[0].actor.save().toJson();
        Canvas canvas;
        const auto allocations = device.texturesCreated;
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        client.draw(canvas, *received);
        canvas.end();
        const auto actual = device.draws;
        CHECK(device.texturesCreated == allocations);
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        host.drawStatus(canvas, players);
        // Labels in disjoint lanes may be submitted in party-vector or seat order.
        for (const auto& runtime : players | std::views::reverse) {
            host.drawSelectors(canvas, messages.text(), &strings, std::span{&runtime, 1});
        }
        canvas.end();
        compare(actual, device.draws);
        CHECK(players[0].actor.save().toJson() == save);
    }
    auto hidden = HudCapture::capture(players, host, false);
    REQUIRE(hidden);
    Canvas canvas;
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    client.draw(canvas, *hidden);
    canvas.end();
    CHECK(device.draws.empty());
    CHECK(hidden->cards.empty());
    CHECK_FALSE(hidden->counts[0]);
    const auto allocations = device.texturesCreated;
    for (s32 character = 0; character < kClassCount; ++character) {
        for (s32 color = 0; color < kColorCount; ++color) {
            auto shown = player();
            shown.character = character;
            shown.color = color;
            for (s32 gleam = 0; gleam < TurboMeter::kGleamFrames; ++gleam) {
                shown.turbo->gleam = gleam;
                HudSnapshot frame;
                frame.players[0] = shown;
                device.draws.clear();
                canvas.begin(device, Mat4{1});
                client.draw(canvas, frame);
                canvas.end();
                CHECK_FALSE(device.draws.empty());
                CHECK(device.texturesCreated == allocations);
            }
        }
    }
    players[0].actor.spawn(0, {}, nullptr, {}, 0);
    CHECK_FALSE(HudCapture::capture(players, host, true)); // Duplicate seat, not reordered lanes.
    client.clear();
    CHECK_FALSE(client.ready());
}

TEST_CASE("replica preloads all native pickup cards and level-specific coin counters",
          "[netplay][replica-hud][assets]") {
    const auto root = test::assetOrSkip("STATIC/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    TextureSet level;
    REQUIRE(level.load(root / "ITEMS/LEVELS"));
    TextureSet artwork;
    REQUIRE(artwork.load(root / "STATIC"));
    PartyHud host;
    REQUIRE(host.load(device, root, nullptr));
    host.setCountTextures(&level);
    ReplicaHud client;
    REQUIRE(client.load(device, root, nullptr));
    HudSnapshot needsCoins;
    needsCoins.counts[0] = HudCount{HudCountKind::Minotaur, 1, 50};
    CHECK_FALSE(client.accepts(needsCoins)); // Never admit art absent from the load barrier.
    REQUIRE(client.load(device, root, nullptr, {&level, nullptr, {}, {}}));
    CHECK(client.accepts(needsCoins));
    Canvas canvas;
    for (u32 id = 0; id < static_cast<u32>(HudCountKind::Count); ++id) {
        const auto icon = ReplicaHud::countTexture(static_cast<HudCountKind>(id));
        CAPTURE(icon);
        REQUIRE((artwork.find(icon).has_value() || level.find(icon).has_value()));
        host.pickups().clear();
        for (u32 card = 0; card < static_cast<u32>(HudCardKind::Count); ++card) {
            REQUIRE(artwork.find(ReplicaHud::cardTexture(static_cast<HudCardKind>(card))));
            host.pickups().addCard(static_cast<s32>(card % 4),
                                   ReplicaHud::cardTexture(static_cast<HudCardKind>(card)));
        }
        for (s32 seat = 0; seat < 4; ++seat) {
            host.pickups().showCount(seat, ReplicaHud::countTexture(static_cast<HudCountKind>(id)),
                                     seat == 0 ? -1 : seat, 250);
        }
        host.pickups().step(80, 0);
        const auto state = HudCapture::capture({}, host, true);
        REQUIRE(state);
        CHECK(state->cards.size() == static_cast<usize>(HudCardKind::Count));
        const auto allocations = device.texturesCreated;
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        client.draw(canvas, *state);
        canvas.end();
        CHECK(device.texturesCreated == allocations);
        const auto actual = device.draws;
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        host.drawStatus(canvas, {});
        canvas.end();
        compare(actual, device.draws);
    }
    host.pickups().addCard(0, "../unexpected-file");
    CHECK_FALSE(HudCapture::capture({}, host, true));
    host.pickups().clear();
    host.pickups().showCount(0, "../unexpected-file", 0, 0);
    CHECK_FALSE(HudCapture::capture({}, host, true));
}

TEST_CASE("replica boss bars use host eased widths and preloaded native layer geometry",
          "[netplay][replica-hud][assets]") {
    const auto root = test::assetOrSkip("STATIC/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    PartyHud host;
    REQUIRE(host.load(device, root, nullptr));
    for (const auto kind : {34, 35, 41}) { // Dragon, Chimera's three heads, Lich.
        CAPTURE(kind);
        Bosses bosses;
        bosses.open(device, root, nullptr, {}, 'A');
        REQUIRE(bosses.spawn(kind, Vec3{0}, 0));
        auto readings = bosses.healthMeters();
        REQUIRE_FALSE(readings.empty());
        REQUIRE(bosses.archive() != nullptr);
        auto& textures = bosses.archive()->textures;
        BossMeters meter;
        meter.bind(readings, &textures);
        ReplicaHud client;
        CHECK_FALSE(client.load(device, root, nullptr, {nullptr, nullptr, readings, {}}));
        REQUIRE(client.load(device, root, nullptr, {nullptr, &textures, readings, {}}));
        CHECK_FALSE(client.accepts(HudSnapshot{}));
        for (s32 tick = 0; tick < 8; ++tick) {
            for (auto& reading : readings) {
                reading.health = tick < 4 ? reading.maxHealth / 3 : 0;
            }
            meter.update(10, readings, tick < 7, tick == 2);
            const auto state = HudCapture::capture({}, host, true, &meter);
            REQUIRE(state);
            REQUIRE(state->bossBars.size() == readings.size());
            const auto bytes = HudPacket::encode(*state);
            REQUIRE(bytes);
            const auto received = HudPacket::decode(*bytes);
            REQUIRE(received);
            Canvas canvas;
            device.draws.clear();
            for (s32 repeat = 0; repeat < 3; ++repeat) { // Draws do not run client-side easing.
                const auto allocations = device.texturesCreated;
                device.draws.clear();
                canvas.begin(device, Mat4{1});
                client.draw(canvas, *received);
                canvas.end();
                CHECK(device.texturesCreated == allocations);
                const auto actual = device.draws;
                device.draws.clear();
                canvas.begin(device, Mat4{1});
                host.drawStatus(canvas, {});
                meter.draw(canvas, device);
                canvas.end();
                compare(actual, device.draws);
            }
        }
        client.clear();
        CHECK_FALSE(client.ready());
    }
}

TEST_CASE("replica screen overlays use native drawing order and host clocks on every aspect",
          "[netplay][replica-hud][assets]") {
    const auto root = test::assetOrSkip("STATIC/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto level = levels.byName("G1");
    REQUIRE(level);
    REQUIRE_FALSE(level->title.empty());
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    PartyHud host;
    REQUIRE(host.load(device, root, &strings));
    TextureSet staticTextures;
    LevelMessages messages;
    messages.load(device, staticTextures, root, &strings);
    REQUIRE(messages.text().ready());
    TransitionScreen transition;
    REQUIRE(transition.load(device, root));
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    LevelArrivalPresentation arrival;
    GameOver over;
    const auto caption = GameOver::captionFor(host.strings(), &strings);
    REQUIRE_FALSE(caption.empty());
    ReplicaHud client;
    REQUIRE(client.load(device, root, &strings));
    HudSnapshot needsTitle;
    needsTitle.screen.titleScale = 1;
    CHECK_FALSE(client.accepts(needsTitle));
    HudResources resources;
    resources.levelTitle = level->title;
    REQUIRE(client.load(device, root, &strings, resources));
    CHECK(client.accepts(needsTitle));
    Canvas canvas;
    for (const auto size : {Vec2{640, 448}, Vec2{1920, 1080}, Vec2{800, 1200}}) {
        CAPTURE(size.x, size.y);
        const Mat4 projection = makeLetterboxProjection(512, 384, size.x, size.y);
        transition.cover();
        transition.clearAway();
        arrival.begin(device, weapons, {}, WorldCamera{});
        over.clear();
        for (s32 tick = 0; tick <= 390; ++tick) {
            CAPTURE(tick);
            if (tick == 70) {
                transition.comeUp();
            }
            if (tick == 150) {
                over.begin(caption);
            }
            const bool cut = tick >= 30 && tick < 60;
            const auto screen = HudCapture::screen(transition, arrival, over, cut);
            auto state = HudCapture::capture({}, host, !cut && !over.active());
            REQUIRE(state);
            state->screen = screen;
            const auto bytes = HudPacket::encode(*state);
            REQUIRE(bytes);
            const auto received = HudPacket::decode(*bytes);
            REQUIRE(received);
            for (s32 repeat = 0; repeat < 2; ++repeat) {
                const auto allocations = device.texturesCreated;
                device.draws.clear();
                canvas.begin(device, projection);
                client.draw(canvas, *received);
                canvas.end();
                const auto actual = device.draws;
                CHECK(device.texturesCreated == allocations);
                device.draws.clear();
                canvas.begin(device, projection);
                if (over.active()) {
                    over.draw(canvas, messages.text(), 512);
                } else {
                    transition.draw(canvas, 512);
                    if (!cut) {
                        host.drawStatus(canvas, {});
                    }
                    arrival.drawTitle(canvas, messages.text(), level->title, 512);
                    if (cut) {
                        drawCinematicBars(canvas, 384);
                    }
                }
                canvas.end();
                compare(actual, device.draws);
                CHECK(HudPacket::encode(*received) == bytes);
            }
            if (!over.active()) {
                transition.update(1.0f / 30);
                arrival.advance(1, tick == 90, {0, 10, -10}, {0, 0, 0});
            } else {
                over.step(1);
            }
        }
    }
    client.clear();
    CHECK_FALSE(client.ready());
    CHECK_FALSE(client.accepts(needsTitle));
}
} // namespace
