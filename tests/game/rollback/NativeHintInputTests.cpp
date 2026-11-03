#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/BitmapFont.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/online/NativeRollbackDriver.h"

namespace {
using namespace gdl;
using namespace gdl::game;

BitmapFont font() {
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = 33; c < 127; ++c) {
        glyphs.push_back({c, 8, 0, 0});
    }
    return BitmapFont::fromGlyphs(10, 4, std::move(glyphs));
}

MenuInput click(const OptionMenu& menu, usize row) {
    const Rect area = menu.itemArea(row);
    MenuInput input;
    input.pointer = Vec2{area.x + area.width * 0.5f, area.y + area.height * 0.5f};
    input.pointerPressed = true;
    return input;
}

struct MenuFixture {
    BitmapFont glyphs = font();
    test::FakeTexture texture{64, 64};
    TextPainter text;
    test::FakeRenderDevice device;
    HintMenu native, shared;
    MenuFixture() {
        text.setFont(&glyphs, &texture);
        HintMenuLabels labels;
        labels.title = "HELP";
        labels.topics = {{"GENERAL", 39}, {"GUARDIANS", 40}, {"LEGENDS", 41}, {"RUNES", 42}};
        REQUIRE(native.open(text, {}, labels));
        REQUIRE(shared.open(text, {}, labels));
    }
};

struct Peer {
    test::FakeRenderDevice device;
    GameConfig config;
    StringTable strings;
    LevelWorld world;
    GameServices context; ///< what the scene reads while it stands
    PlayScene scene;
    MatchContext match;
    u8 id;
    NativeRollbackDriver driver;
    NativeRollbackDriver::Result last;
    Peer(u8 peer, u8 delay) : id(peer) {
        device.recordDraws = false;
        config.timing.tickRate = 60;
        const auto root = test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2")
                              .parent_path()
                              .parent_path()
                              .parent_path();
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        config.camera.horizontalFovDegrees = peer == 1 ? 42.0f : 97.0f;
        context.config = &config;
        context.strings = &strings;
        context.unpackedRoot = root;
        std::array<PartyMember, 2> party;
        party[0].player = 1;
        party[1].player = 3;
        for (auto& member : party) {
            member.save.name = "TEST";
            member.save.character = 0;
            member.save.progress().health = 2000;
        }
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{3.3f, 2.1f, -49.0f};
        REQUIRE(scene.open(device, context, world, party, options));
        // The same actual native starting placement on both peers: guest seat3
        // is the visitor, and host seat1 is away from Sumner's authored trigger.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        const_cast<PlayerActor*>(scene.actor(1))->place(Vec3{0, 2.1f, 0});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        const_cast<PlayerActor*>(scene.actor(3))->place(*options.position);
        match.epoch = 1;
        match.scene = 1;
        match.owners = {0, 1, 0, 2};
        match.grants = {0, 1, 0, 2};
        match.inputLead = delay;
        REQUIRE(scene.beginRecordedAudio(1));
        REQUIRE(driver.bind(scene, match, id, NativeRollbackDriver::AudioMode::Required));
    }
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    ~Peer() {
        driver.clear();
        scene.endRecordedAudio();
    }
    NativeRollbackDriver::Status advance(bool forward = true) {
        last = driver.advance(forward);
        return last.status;
    }
};

struct Pair {
    std::unique_ptr<Peer> host, guest;
    explicit Pair(u8 delay)
        : host(std::make_unique<Peer>(1, delay)), guest(std::make_unique<Peer>(2, delay)) {
        for (u32 tick = 0; tick < 900 && !host->scene.hints().active(); ++tick) {
            step();
        }
        REQUIRE(host->scene.hints().active());
        REQUIRE(guest->scene.hints().active());
        REQUIRE(host->scene.hints().topics().definition().playerLabel == "Player 4");
    }
    void pump() {
        using A = NativeRollbackDriver::Admission;
        for (const bool reverse : {false, true}) {
            auto& from = reverse ? *guest : *host;
            auto& to = reverse ? *host : *guest;
            if (!from.driver.redundantInputs().empty()) {
                const auto result = to.driver.submit(from.id, from.driver.redundantInputs());
                REQUIRE((result == A::Accepted || result == A::Duplicate || result == A::Stale));
            }
            const auto result = to.driver.observePeerTick(from.id, 1, from.driver.nextTick());
            REQUIRE((result == A::Accepted || result == A::Duplicate || result == A::Stale));
        }
    }
    void step(const SessionInputs::Frame& first = {}, const SessionInputs::Frame& second = {}) {
        REQUIRE(host->driver.sample(first));
        REQUIRE(guest->driver.sample(second));
        pump();
        REQUIRE(host->advance() == NativeRollbackDriver::Status::Advanced);
        REQUIRE(guest->advance() == NativeRollbackDriver::Status::Advanced);
    }
};
} // namespace

TEST_CASE("semantic Sumner choices preserve native pointer selection and cursor timing",
          "[game][hint-input]") {
    const auto row = GENERATE(usize{0}, usize{1}, usize{2}, usize{3});
    const auto ticks = GENERATE(1, 2);
    MenuFixture f;
    for (s32 frame = 0; frame < 3; ++frame) {
        f.native.update(f.device, {}, ticks);
        f.shared.update(f.device, {}, ticks);
    }
    const auto pointer = click(f.native.topics(), row);
    const u8 topic = f.shared.captureTopic(pointer);
    REQUIRE(topic == row + 1);
    const auto expected = f.native.update(f.device, pointer, ticks);
    const auto actual = f.shared.update(f.device, {}, ticks, topic, f.shared.revision());
    REQUIRE(expected.kind == HintMenuEvent::Kind::Asked);
    CHECK(actual.kind == expected.kind);
    CHECK(actual.topic == expected.topic);
    for (s32 frame = 0; frame < 12; ++frame) {
        CHECK(f.shared.topics().selection() == f.native.topics().selection());
        CHECK(f.shared.topics().time() == f.native.topics().time());
        CHECK(f.shared.topics().iconY() == f.native.topics().iconY());
        CHECK(f.shared.topics().iconAngle() == f.native.topics().iconAngle());
        f.native.update(f.device, {}, ticks);
        f.shared.update(f.device, {}, ticks);
    }
    auto outside = pointer;
    outside.pointer = Vec2{-100, -100};
    CHECK(f.shared.captureTopic(outside) == 0);
    outside = pointer;
    outside.pointerNormalized = true;
    CHECK(f.shared.captureTopic(outside) == 0);
    outside = pointer;
    outside.pointerPressed = false;
    CHECK(f.shared.captureTopic(outside) == 0);
}

TEST_CASE("Sumner topic revisions reject old pages backs closes and tolerate byte wrap",
          "[game][hint-input]") {
    MenuFixture f;
    const auto original = f.shared.revision();
    f.shared.read(f.text, "ANSWER", {"TEXT"}, 1, true, 0);
    CHECK_FALSE(f.shared.acceptsTopic(1, original));
    CHECK(f.shared.update(f.device, {}, 1, 1, original).kind == HintMenuEvent::Kind::None);
    MenuInput back;
    back.back = true;
    REQUIRE(f.shared.update(f.device, back, 1).kind == HintMenuEvent::Kind::Returned);
    CHECK_FALSE(f.shared.acceptsTopic(1, original));
    CHECK(f.shared.update(f.device, {}, 1, 1, original).kind == HintMenuEvent::Kind::None);
    CHECK_FALSE(f.shared.acceptsTopic(5, f.shared.revision()));
    bool sawZero = false;
    for (s32 page = 0; page < 256; ++page) {
        f.shared.read(f.text, "ANSWER", {"TEXT"}, 1, true, 0);
        REQUIRE(f.shared.update(f.device, back, 1).kind == HintMenuEvent::Kind::Returned);
        if (f.shared.revision() == 0) {
            sawZero = true;
            CHECK(f.shared.acceptsTopic(1, 0));
        }
    }
    REQUIRE(sawZero);
    const auto current = f.shared.revision();
    f.shared.close();
    CHECK_FALSE(f.shared.acceptsTopic(1, current));
    CHECK(f.shared.captureTopic(click(f.native.topics(), 0)) == 0);
}

TEST_CASE("native sparse-seat Sumner mouse clicks survive a stall exactly once",
          "[native-rollback-lifecycle][hint-input][assets]") {
    Pair pair(0);
    const auto& menu = pair.guest->scene.hints();
    const auto pointer = click(menu.topics(), 1);
    PlayInput clickInput;
    clickInput.menu = pointer;
    CHECK(pair.host->scene.captureNativeInput(1, clickInput).hintTopic == 0);
    CHECK(pair.guest->scene.captureNativeInput(3, clickInput).hintTopic == 2);

    // Only the guest renders. Resolve a normalized point through its actually
    // displayed letterbox/canvas; the other peer only receives the semantic row.
    const auto projection = makeLetterboxProjection(640, 480, 1280, 720);
    const auto canvas = makeVirtualScreenTransform(
        projection, static_cast<f32>(pair.guest->config.display.virtualWidth),
        static_cast<f32>(pair.guest->config.display.virtualHeight), 640, 480);
    const Vec4 clip = canvas * Vec4{*pointer.pointer, 0.5f, 1};
    clickInput.menu.pointer = Vec2{clip} / clip.w * 0.5f + Vec2{0.5f};
    clickInput.menu.pointerNormalized = true;
    CHECK(pair.guest->scene.captureNativeInput(3, clickInput).hintTopic == 0);
    pair.guest->scene.render(pair.guest->device, projection, 640, 480);
    REQUIRE(pair.guest->scene.captureNativeInput(3, clickInput).hintTopic == 2);

    REQUIRE(pair.guest->driver.sample({}));
    const auto heldTick = (pair.guest->driver.nextTick() - 1);
    for (s32 poll = 0; poll < 20; ++poll) {
        SessionInputs::Frame local;
        if (poll == 3) {
            local[0] = clickInput; // local device0 owns canonical seat3
        }
        REQUIRE(pair.guest->driver.sample(local));
        REQUIRE(pair.guest->advance(false) == NativeRollbackDriver::Status::Held);
        CHECK((pair.guest->driver.nextTick() - 1) == heldTick);
    }
    REQUIRE(pair.host->driver.sample({}));
    pair.pump();
    REQUIRE(pair.host->advance() == NativeRollbackDriver::Status::Advanced);
    REQUIRE(pair.guest->advance() == NativeRollbackDriver::Status::Advanced);
    pair.step();
    for (const auto* peer : {pair.host.get(), pair.guest.get()}) {
        REQUIRE_FALSE(peer->last.frames.empty());
        CHECK(peer->last.frames.back().input.commands[3].hintTopic == 2);
        REQUIRE(peer->scene.hints().reading());
        CHECK(peer->scene.hints().page().definition().title == "The Lich");
    }
    SessionInputs::Frame back;
    back[0].menu.back = true;
    pair.step({}, back);
    REQUIRE_FALSE(pair.host->scene.hints().reading());
    for (s32 tick = 0; tick < 8; ++tick) {
        pair.step();
        CHECK_FALSE(pair.host->scene.hints().reading());
        CHECK_FALSE(pair.guest->scene.hints().reading());
        CHECK(pair.guest->last.frames.back().input.commands[3].hintTopic == 0);
    }
    CHECK(pair.host->scene.hints().topics().selection() == 1);
    CHECK(pair.guest->scene.hints().topics().selection() == 1);
}

TEST_CASE("Sumner checks the semantic sender against the live menu owner",
          "[game][hint-input][assets]") {
    MenuFixture f;
    SumnerVisit visit;
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    REQUIRE(visit.loadTexts(root / "text/hints_e.json"));
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    REQUIRE(visit.visit(0, 3, true, f.text, &config, &strings));
    CHECK_FALSE(visit.visit(SumnerVisit::kGreetingSeconds, 3, true, f.text, &config, &strings));
    REQUIRE(visit.owner() == 3);
    const auto pointer = click(visit.menu().topics(), 0);
    CHECK(visit.captureTopic(1, pointer) == 0);
    REQUIRE(visit.captureTopic(3, pointer) == 1);
    const u8 revision = visit.menu().revision();
    MenuInput select;
    select.select = true;
    CHECK(visit.updateNative(f.device, 1, select, 1, 1, revision).kind ==
          HintMenuEvent::Kind::None);
    CHECK(visit.updateNative(f.device, 3, {}, 1, 1, static_cast<u8>(revision + 1)).kind ==
          HintMenuEvent::Kind::None);
    const auto chosen = visit.updateNative(f.device, 3, {}, 1, 1, revision);
    REQUIRE(chosen.kind == HintMenuEvent::Kind::Asked);
    REQUIRE(chosen.topic == 39);
    visit.answer(chosen.topic, f.text, &strings, {});
    REQUIRE(visit.menu().reading());
    CHECK(visit.updateNative(f.device, 3, {}, 1, 1, revision).kind == HintMenuEvent::Kind::None);
}
