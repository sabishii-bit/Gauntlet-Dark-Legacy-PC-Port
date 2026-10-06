#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/Bosses.h"
#include "game/screens/LegendPresentation.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** Synthetic triangle effects keep lifecycle tests independent of installed game data. */
std::filesystem::path legendArchive(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"defs":[],"bitmaps":[
        {"index":0,"name":"SEETHROUGH","file":"skin.png","width":2,"height":2,"flags":0},
        {"index":1,"name":"PARTICLE1_A","file":"skin.png","width":2,"height":2,"flags":0}]})");
    test::convertModelFixture(dir);
    std::string trees;
    for (const std::string_view tree :
         {"LEGENDHLD", "LEGENDPRJ", "LEGENDFX", "LEGENDFX2", "COMBO_SPH", "COMBO_BLU"}) {
        if (!trees.empty()) {
            trees += ',';
        }
        trees += std::format(R"({{"name":"{}","prefix":"","sequences":[],"nodes":[
            {{"name":"BODY","object":"BODY","type":0,"flags":0,"objectFlags":0,
              "parent":-1,"position":[0,0,0]}}]}})",
                             tree);
    }
    writeTextFile(dir / "animations.json", "{\"trees\":[" + trees + "]}");
    return dir;
}

struct LegendFixture {
    test::FakeRenderDevice device;
    ItemArchive items;
    ItemArchive weapons;
    TextureSet shared;
    EffectTrees effects;
    std::vector<std::string> sounds;
    std::vector<SoundHandle> stopped;
    LegendPresentation presentation{effects,
                                    {device, items, weapons, shared},
                                    {[this](std::string_view name) {
                                         sounds.emplace_back(name);
                                         return static_cast<SoundHandle>(sounds.size());
                                     },
                                     [this](SoundHandle handle) { stopped.push_back(handle); }}};
    LegendPresentation::Bearer bearer{
        2, 1, Vec3{0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 8.0f, 0.0f}, true, false, false};
    LegendPresentation::Target target{Vec3{20.0f, 0.0f, 1.0f}, 4.0f};

    void load(std::string_view name) { REQUIRE(items.load(legendArchive(name))); }
    void show(LegendCue cue, s32 kind = 34) {
        presentation.show(cue, bearer.player, 2, kind, bearer);
    }
    const EffectTrees::Effect* find(std::string_view name) const {
        for (usize i = 0; i < effects.count(); ++i) {
            if (effects.effect(i).name == name) {
                return &effects.effect(i);
            }
        }
        return nullptr;
    }
};

TEST_CASE("legend presentation retries the gesture until the bearer accepts it",
          "[game][screens][legend]") {
    LegendFixture fixture;
    fixture.show(LegendCue::Brandished);
    REQUIRE(fixture.presentation.player() == 2); // player id, not a scene array index
    fixture.show(LegendCue::Thrown);
    for (s32 i = 0; i < 3; ++i) {
        const auto result = fixture.presentation.update(0.1f, fixture.bearer, fixture.target);
        REQUIRE(result.gesture == PlayerDeed::ThrowLegend);
        REQUIRE_FALSE(result.landed);
    }
    auto other = fixture.bearer;
    other.player = 0;
    REQUIRE(fixture.presentation.update(0.1f, other, fixture.target).gesture == PlayerDeed::None);
    fixture.bearer.casting = true;
    REQUIRE(fixture.presentation.update(0.1f, fixture.bearer, fixture.target).gesture ==
            PlayerDeed::None);
    fixture.bearer.casting = false;
    REQUIRE(fixture.presentation.update(0.1f, fixture.bearer, fixture.target).gesture ==
            PlayerDeed::None);
    fixture.presentation.clear();
    REQUIRE(fixture.presentation.player() == -1);
}

TEST_CASE("carried relics precede the charge and inherit their complete parent transform",
          "[game][screens][legend][legend-held]") {
    const s32 kind = GENERATE(34, 35, 36, 37, 38, 39, 40, 41, 42);
    LegendFixture fixture;
    fixture.load("legend-carried");
    REQUIRE(fixture.weapons.load(legendArchive("legend-carried-weapons")));
    fixture.bearer.holdTransform = glm::scale(
        glm::rotate(glm::translate(Mat4{1}, Vec3{4, 9, 2}), 0.75f, Vec3{0, 0, 1}), Vec3{1.3f});
    fixture.presentation.carry(2, legendRealmOf(kind), kind, fixture.bearer);
    const auto* held = fixture.find("LEGENDHLD");
    REQUIRE(held);
    const u32 id = held->id;
    CHECK(held->transform() == *fixture.bearer.holdTransform);
    CHECK(fixture.presentation.occupiedHand() == (kind <= 39 ? 2 : -1));
    CHECK(fixture.effects.count() == 1);
    CHECK(fixture.sounds.empty());
    CHECK(fixture.presentation.update(0, fixture.bearer, fixture.target).gesture ==
          PlayerDeed::None);
    fixture.effects.update(2);
    fixture.presentation.carry(2, legendRealmOf(kind), kind, fixture.bearer);
    REQUIRE(fixture.find("LEGENDHLD"));
    CHECK(fixture.find("LEGENDHLD")->id == id);
    CHECK(fixture.find("LEGENDHLD")->secondsLeft < LegendShow::kHeldSeconds);
    fixture.show(LegendCue::Brandished, kind);
    CHECK(fixture.find("LEGENDHLD")->id == id);
    CHECK(fixture.effects.count() == 3);
    CHECK(fixture.sounds == std::vector<std::string>{"S_LEGWPUP"});
    fixture.show(LegendCue::Brandished, kind);
    CHECK(fixture.effects.count() == 3);
    CHECK(fixture.sounds.size() == 1);
    fixture.bearer.holdTransform = glm::rotate(*fixture.bearer.holdTransform, 1.0f, Vec3{1, 0, 0});
    fixture.presentation.update(0, fixture.bearer, fixture.target);
    CHECK(fixture.find("LEGENDHLD")->transform() == *fixture.bearer.holdTransform);
    SECTION("the animation release removes the held item and frees the hand") {
        fixture.show(LegendCue::Thrown, kind);
        fixture.bearer.released = true;
        fixture.presentation.update(0, fixture.bearer, fixture.target);
        CHECK(fixture.find("LEGENDHLD") == nullptr);
        CHECK(fixture.presentation.occupiedHand() == -1);
    }
    SECTION("losing the bearer removes the held item without creating a projectile") {
        fixture.presentation.update(0, std::nullopt, fixture.target);
        CHECK(fixture.find("LEGENDHLD") == nullptr);
        CHECK(fixture.find("LEGENDPRJ") == nullptr);
        CHECK(fixture.presentation.occupiedHand() == -1);
    }
    fixture.presentation.clear();
    CHECK(fixture.effects.count() == 0);
    CHECK(fixture.presentation.occupiedHand() == -1);
}

TEST_CASE("all nine native boss archives render a carried legend before brandishing",
          "[game][screens][legend][legend-held][native-assets][assets]") {
    const auto root =
        test::assetOrSkip("ITEMS/LEVELK5/ANIM.PS2").parent_path().parent_path().parent_path();
    const auto [kind, level] =
        GENERATE(std::pair{34, "B6"}, std::pair{35, "A5"}, std::pair{36, "C5"}, std::pair{37, "D5"},
                 std::pair{38, "K5"}, std::pair{39, "I5"}, std::pair{40, "J5"}, std::pair{41, "G5"},
                 std::pair{42, "E2"});
    CAPTURE(kind, level);
    LegendFixture fixture;
    REQUIRE(fixture.items.load(root / "ITEMS" / (std::string{"LEVEL"} + level)));
    fixture.bearer.holdTransform =
        glm::rotate(glm::translate(Mat4{1}, Vec3{3, 5, 8}), 0.5f, Vec3{0, 1, 0});
    fixture.presentation.carry(2, legendRealmOf(kind), kind, fixture.bearer);
    REQUIRE(fixture.find("LEGENDHLD"));
    fixture.effects.draw(fixture.device, Mat4{1}, {});
    REQUIRE_FALSE(fixture.device.draws.empty());
    CHECK(fixture.find("LEGENDHLD")->transform() == *fixture.bearer.holdTransform);
    CHECK(fixture.presentation.occupiedHand() == (kind <= 39 ? 2 : -1));
    CHECK(fixture.sounds.empty());
}

TEST_CASE("legend flight follows its held pose and reports impact exactly once",
          "[game][screens][legend]") {
    LegendFixture fixture;
    fixture.load("legend-flight");
    fixture.show(LegendCue::Brandished);
    REQUIRE(fixture.find("LEGENDHLD") != nullptr);
    fixture.bearer.holdPoint.x = 3.0f;
    fixture.presentation.update(0.0f, fixture.bearer, fixture.target);
    REQUIRE(fixture.find("LEGENDHLD")->position == fixture.bearer.holdPoint);
    fixture.show(LegendCue::Thrown);
    fixture.bearer.casting = true;
    fixture.bearer.released = true;
    REQUIRE_FALSE(fixture.presentation.update(0.0f, fixture.bearer, fixture.target).landed);
    REQUIRE(fixture.find("LEGENDHLD") == nullptr);
    REQUIRE(fixture.find("LEGENDPRJ") != nullptr);
    const u32 flight = fixture.find("LEGENDPRJ")->id;
    REQUIRE(fixture.find("LEGENDPRJ")->velocity == Vec3{20.0f, 0.0f, 0.0f});
    REQUIRE(fixture.find("LEGENDPRJ")->trails.size() == 1);
    // Repeated animation snapshots neither spawn again nor reset the flight clock.
    REQUIRE_FALSE(fixture.presentation.update(0.9f, fixture.bearer, fixture.target).landed);
    REQUIRE(fixture.find("LEGENDPRJ")->id == flight);
    REQUIRE(fixture.presentation.update(0.11f, fixture.bearer, fixture.target).landed);
    REQUIRE(fixture.find("LEGENDPRJ") == nullptr);
    REQUIRE(fixture.find("LEGENDFX") != nullptr);
    REQUIRE(fixture.stopped == std::vector<SoundHandle>{3});
    REQUIRE(fixture.sounds.back() == "S_BLEGWHIT");
    REQUIRE(fixture.presentation.frozenTexture() ==
            &fixture.items.textures.texture(fixture.device, 0));
    REQUIRE_FALSE(fixture.presentation.update(10.0f, fixture.bearer, fixture.target).landed);
    REQUIRE(fixture.stopped.size() == 1);
}

TEST_CASE("carried Fire Parchment retains its native fire", "[legend][fire-parchment][assets]") {
    LegendFixture fixture;
    REQUIRE(fixture.items.load(test::assetOrSkip("ITEMS/LEVELI5/ANIM.PS2").parent_path()));
    fixture.presentation.carry(2, 9, 39, fixture.bearer);
    const auto* held = fixture.find("LEGENDHLD");
    REQUIRE(held);
    for (s32 tick = 0; tick < 60; ++tick) {
        fixture.effects.update(1.0f / 30);
    }
    REQUIRE(held->particles.field().particleCount() > 0);
    const auto slot = fixture.items.textures.find("POOLFIRE");
    REQUIRE(slot);
    const auto* fire = &fixture.items.textures.texture(fixture.device, *slot);
    fixture.effects.draw(fixture.device, Mat4{1}, {});
    CHECK(std::ranges::any_of(fixture.device.draws,
                              [fire](const auto& draw) { return draw.texture == fire; }));
    fixture.presentation.clear();
    CHECK(fixture.effects.count() == 0);
}

TEST_CASE("the Plague javelin follows the animated eye rather than a launch-time timer",
          "[game][screens][legend][plague]") {
    LegendFixture fixture;
    fixture.load("legend-plague-homing");
    fixture.show(LegendCue::Brandished, 38);
    fixture.show(LegendCue::Thrown, 38);
    fixture.bearer.released = true;
    REQUIRE_FALSE(fixture.presentation.update(0, fixture.bearer, fixture.target).landed);
    const auto* flight = fixture.find("LEGENDPRJ");
    REQUIRE(flight != nullptr);
    CHECK(flight->position == Vec3{0, 2, 1});
    fixture.effects.update(0.25f);
    REQUIRE_FALSE(fixture.presentation.update(0.25f, fixture.bearer, fixture.target).landed);
    CHECK(flight->position == Vec3{5, 2, 1});
    CHECK(flight->flightDirection == Vec3{1, 0, 0});
    fixture.target.position = {5, 12, 1};
    fixture.target.height = 0;
    fixture.effects.update(0.25f);
    REQUIRE_FALSE(fixture.presentation.update(0.25f, fixture.bearer, fixture.target).landed);
    CHECK(flight->position == Vec3{5, 7, 1});
    CHECK(flight->flightDirection == Vec3{0, 1, 0});
    SECTION("the projectile survives losing its bearer and lands exactly once") {
        fixture.effects.update(0.25f);
        CHECK(fixture.presentation.update(0.25f, std::nullopt, fixture.target).landed);
        REQUIRE(fixture.find("LEGENDPRJ") == nullptr);
        REQUIRE(fixture.find("LEGENDFX") != nullptr);
        CHECK(fixture.find("LEGENDFX")->position == fixture.target.position);
        CHECK_FALSE(fixture.presentation.update(1, std::nullopt, fixture.target).landed);
    }
    SECTION("a lost target expires without reporting an impact") {
        fixture.effects.update(6);
        CHECK_FALSE(fixture.presentation.update(6, std::nullopt, std::nullopt).landed);
        CHECK(fixture.find("LEGENDPRJ") == nullptr);
        CHECK(fixture.find("LEGENDFX") == nullptr);
        CHECK(fixture.stopped.size() == 1);
    }
}

TEST_CASE("the Plague eye recovers without inventing another relic sound",
          "[game][screens][legend][plague]") {
    LegendFixture fixture;
    fixture.show(LegendCue::Brandished, 38);
    const auto sounds = fixture.sounds;
    fixture.show(LegendCue::WornOff, 38);
    CHECK(fixture.sounds == sounds);
}

TEST_CASE("native Plague javelin hits the body and leaves no projectile chasing the injured eye",
          "[game][screens][legend][plague][native-assets][assets]") {
    const auto root = test::assetOrSkip("CRITTER/PBOSS.WAD").parent_path().parent_path();
    LegendFixture fixture;
    REQUIRE(fixture.items.load(root / "ITEMS/LEVELK5"));
    REQUIRE(fixture.weapons.load(root / "WEAPONS"));
    Bosses bosses;
    bosses.open(fixture.device, root, nullptr, {}, 'K');
    REQUIRE(bosses.spawn(38, Vec3{0}, 0, 100));
    REQUIRE(bosses.bringLegend(fixture.bearer.player));
    fixture.bearer.position = {0, 0, 44};
    fixture.bearer.facing = {0, 0, -1};
    EnemyView player;
    player.player = fixture.bearer.player;
    player.position = fixture.bearer.position;
    player.height = 6;
    const std::array players{player};
    constexpr f32 kStep = 1.0f / 60;
    for (s32 tick = 0; tick < 1800 && !bosses.legend().thrown(); ++tick) {
        bosses.update(1, kStep, players);
    }
    REQUIRE(bosses.legend().thrown());
    fixture.show(LegendCue::Brandished, 38);
    fixture.show(LegendCue::Thrown, 38);
    fixture.bearer.released = true;
    bool landed = false;
    f32 remainingDistance = 0;
    for (s32 tick = 0; tick < 360 && !landed; ++tick) {
        bosses.update(1, kStep, players);
        const auto eye = bosses.nodeTransform("BODY1_EYEBALL");
        REQUIRE(eye);
        fixture.target.position = Vec3{(*eye)[3]};
        fixture.target.height = 0;
        fixture.target.touches = [&](const Vec3& from, const Vec3& to, f32 radius) {
            remainingDistance = glm::distance(to, fixture.target.position);
            return bosses.struckBy(from, to, radius).has_value();
        };
        landed = fixture.presentation.update(kStep, fixture.bearer, fixture.target).landed;
        fixture.effects.update(kStep);
    }
    REQUIRE(landed);
    CHECK(remainingDistance > LegendShow::kSpeed * kStep);
    CHECK(fixture.find("LEGENDPRJ") == nullptr);
    // PBOSS has no LEGENDFX. The embedded weapon is PBOSSQEYEBALL, which
    // Combatant draws at the eye's posed transform, not a world-space effect.
    CHECK(fixture.find("LEGENDFX") == nullptr);
    bosses.landLegend();
    CHECK(bosses.blinded());
    for (s32 tick = 0; tick < 120; ++tick) {
        bosses.update(1, kStep, players);
        fixture.presentation.update(kStep, fixture.bearer, fixture.target);
        fixture.effects.update(kStep);
        CHECK(fixture.find("LEGENDPRJ") == nullptr);
    }
}

TEST_CASE("the Plague javelin lands on swept body contact before reaching the eye centre",
          "[game][screens][legend][plague]") {
    LegendFixture fixture;
    fixture.load("legend-plague-contact");
    fixture.show(LegendCue::Brandished, 38);
    fixture.show(LegendCue::Thrown, 38);
    fixture.bearer.released = true;
    fixture.presentation.update(0, fixture.bearer, fixture.target);
    s32 sweeps = 0;
    fixture.target.touches = [&](const Vec3& from, const Vec3& to, f32 radius) {
        ++sweeps;
        CHECK(from == Vec3{0, 2, 1});
        CHECK(to == Vec3{5, 2, 1});
        CHECK(radius == 2);
        return true;
    };
    CHECK(fixture.presentation.update(0.25f, fixture.bearer, fixture.target).landed);
    CHECK(sweeps == 1);
    CHECK(fixture.find("LEGENDPRJ") == nullptr);
    fixture.target.position = {100, 40, 1};
    CHECK_FALSE(fixture.presentation.update(1, fixture.bearer, fixture.target).landed);
    CHECK(sweeps == 1);
}

TEST_CASE("an unavailable Plague body cannot be hit just by reaching its homing anchor",
          "[game][screens][legend][plague]") {
    LegendFixture fixture;
    fixture.load("legend-plague-missing-body");
    fixture.show(LegendCue::Brandished, 38);
    fixture.show(LegendCue::Thrown, 38);
    fixture.bearer.released = true;
    fixture.target.touches = [](const Vec3&, const Vec3&, f32) { return false; };
    CHECK_FALSE(fixture.presentation.update(2, fixture.bearer, fixture.target).landed);
    const auto* flight = fixture.find("LEGENDPRJ");
    REQUIRE(flight);
    CHECK(flight->position == fixture.target.position + Vec3{0, 2, 0});
    // Standing exactly on the anchor must neither divide by zero nor report a hit.
    CHECK_FALSE(fixture.presentation.update(1, fixture.bearer, fixture.target).landed);
    CHECK_FALSE(flight->flightDirection);
    CHECK_FALSE(fixture.presentation.update(4, fixture.bearer, fixture.target).landed);
    CHECK(fixture.find("LEGENDPRJ") == nullptr);
}

TEST_CASE("the genie lamp's blindness effect rides its root for 28 seconds",
          "[game][screens][legend][genie]") {
    LegendFixture fixture;
    fixture.load("legend-genie-blindness");
    fixture.show(LegendCue::Brandished, 36);
    fixture.show(LegendCue::Thrown, 36);
    fixture.bearer.released = true;
    fixture.target.root = glm::translate(Mat4{1}, Vec3{20, 7, 1});
    fixture.presentation.update(0, fixture.bearer, fixture.target);
    REQUIRE(fixture.presentation.update(2, fixture.bearer, fixture.target).landed);
    const auto* effect = fixture.find("LEGENDFX");
    REQUIRE(effect != nullptr);
    REQUIRE(effect->secondsLeft == 28);
    REQUIRE(Vec3{effect->transform()[3]} == Vec3{20, 13, 1});
    fixture.target.root = glm::translate(Mat4{1}, Vec3{22, 8, 1});
    // The effect follows the boss even if its bearer is no longer present.
    fixture.presentation.update(0, std::nullopt, fixture.target);
    REQUIRE(Vec3{fixture.find("LEGENDFX")->transform()[3]} == Vec3{22, 14, 1});
    fixture.effects.update(27.9f);
    REQUIRE(fixture.find("LEGENDFX") != nullptr);
    fixture.effects.update(0.2f);
    REQUIRE(fixture.find("LEGENDFX") == nullptr);
}

TEST_CASE("Bellows release once and their plume turns with the bearer", "[legend][spider]") {
    LegendFixture fixture;
    fixture.load("legend-bellows");
    fixture.show(LegendCue::Brandished, 37);
    fixture.show(LegendCue::Thrown, 37);
    REQUIRE_FALSE(fixture.presentation.update(0, fixture.bearer, fixture.target).landed);
    fixture.bearer.position = {2, 3, 4};
    fixture.bearer.facing = {1, 0, 0};
    fixture.bearer.released = true;
    REQUIRE(fixture.presentation.update(0, fixture.bearer, fixture.target).landed);
    const auto* effect = fixture.find("LEGENDPRJ");
    REQUIRE(effect != nullptr);
    REQUIRE(effect->secondsLeft == 3);
    REQUIRE(effect->position.x == Approx(7));
    REQUIRE(effect->position.y == Approx(3));
    REQUIRE(effect->position.z == Approx(4));
    REQUIRE(effect->transform()[2].x == Approx(1));
    fixture.bearer.facing = {0, 0, -1};
    REQUIRE_FALSE(fixture.presentation.update(0, fixture.bearer, fixture.target).landed);
    REQUIRE(effect->position.z == Approx(-1));
    REQUIRE(effect->transform()[2].z == Approx(-1));
    fixture.effects.update(3.1f);
    REQUIRE(fixture.effects.count() == 0);
}

TEST_CASE("Savior releases once at Skorne's chest and its effect lasts thirty seconds",
          "[game][screens][legend][skorne]") {
    LegendFixture fixture;
    fixture.load("legend-savior");
    const auto show = [&](LegendCue cue) {
        fixture.presentation.show(cue, fixture.bearer.player, 5, 42, fixture.bearer);
    };
    show(LegendCue::Brandished);
    show(LegendCue::Thrown);
    REQUIRE(fixture.presentation.update(0, fixture.bearer, fixture.target).gesture ==
            PlayerDeed::HurlLegend);
    REQUIRE(fixture.find("LEGENDPRJ") == nullptr);
    fixture.bearer.casting = true;
    REQUIRE_FALSE(fixture.presentation.update(0, fixture.bearer, fixture.target).landed);
    fixture.bearer.released = true;
    REQUIRE(fixture.presentation.update(0, fixture.bearer, fixture.target).landed);
    const auto* effect = fixture.find("LEGENDPRJ");
    REQUIRE(effect != nullptr);
    REQUIRE(effect->position == fixture.target.position + Vec3{0, 17, 3});
    REQUIRE(effect->secondsLeft == 30);
    REQUIRE_FALSE(fixture.presentation.update(0, fixture.bearer, fixture.target).landed);
    REQUIRE(fixture.sounds == std::vector<std::string>{"S_LEGWPUP", "S_ELEGWTHROW", "S_ELEGWFLY"});
    fixture.effects.update(0);
    REQUIRE(fixture.find("LEGENDPRJ") == nullptr);
    REQUIRE(fixture.find("LEGENDFX") != nullptr);
    fixture.effects.update(29.9f);
    REQUIRE(fixture.find("LEGENDFX") != nullptr);
    fixture.effects.update(0.2f);
    REQUIRE(fixture.find("LEGENDFX") == nullptr);
    show(LegendCue::WornOff);
    REQUIRE(fixture.stopped == std::vector<SoundHandle>{3});
    REQUIRE(fixture.sounds.back() == "S_ELEGWPDN");
}

TEST_CASE("legend cleanup releases its own effects and sound but not other effects",
          "[game][screens][legend]") {
    LegendFixture fixture;
    fixture.load("legend-cleanup");
    REQUIRE(fixture.weapons.load(legendArchive("legend-cleanup-weapons")));
    EffectTrees::Setting setting;
    setting.seconds = 60.0f;
    const u32 unrelated =
        fixture.effects.startSet(fixture.device, fixture.items, "LEGENDFX2", Vec3{0.0f}, setting);
    fixture.show(LegendCue::Brandished);
    REQUIRE(fixture.find("COMBO_SPH") != nullptr);
    REQUIRE(fixture.find("COMBO_BLU") != nullptr);
    fixture.show(LegendCue::Thrown);
    fixture.bearer.canGesture = false;
    const auto result = fixture.presentation.update(0.0f, fixture.bearer, fixture.target);
    REQUIRE(result.gesture == PlayerDeed::None);
    REQUIRE(fixture.find("LEGENDPRJ") != nullptr); // no body: release immediately
    fixture.presentation.clear();
    REQUIRE(fixture.effects.count() == 1);
    REQUIRE(fixture.effects.playing(unrelated));
    REQUIRE(fixture.stopped == std::vector<SoundHandle>{3});
    fixture.presentation.clear();
    REQUIRE(fixture.stopped.size() == 1);
    REQUIRE(fixture.presentation.frozenTexture() == nullptr);
}

TEST_CASE("legend presentation destruction stops a live flight", "[game][screens][legend]") {
    LegendFixture fixture;
    fixture.load("legend-destruction");
    {
        LegendPresentation presentation{
            fixture.effects,
            {fixture.device, fixture.items, fixture.weapons, fixture.shared},
            {[](std::string_view) { return SoundHandle{17}; },
             [&fixture](SoundHandle handle) { fixture.stopped.push_back(handle); }}};
        presentation.show(LegendCue::Brandished, 2, 2, 34, fixture.bearer);
        presentation.show(LegendCue::Thrown, 2, 2, 34, fixture.bearer);
        fixture.bearer.canGesture = false;
        presentation.update(0.0f, fixture.bearer, fixture.target);
        REQUIRE(fixture.find("LEGENDPRJ") != nullptr);
    }
    REQUIRE(fixture.effects.count() == 0);
    REQUIRE(fixture.stopped == std::vector<SoundHandle>{17});
}

TEST_CASE("a bearer-bound legend follows the bearer without emitting an impact",
          "[game][screens][legend]") {
    LegendFixture fixture;
    fixture.load("legend-bearer");
    fixture.show(LegendCue::Brandished, 37);
    fixture.show(LegendCue::Thrown, 37);
    fixture.bearer.released = true;
    fixture.presentation.update(0.0f, fixture.bearer, fixture.target);
    fixture.bearer.position.x = 7.0f;
    REQUIRE_FALSE(fixture.presentation.update(0.1f, fixture.bearer, fixture.target).landed);
    REQUIRE(fixture.find("LEGENDPRJ") != nullptr);
    REQUIRE(fixture.find("LEGENDPRJ")->position == Vec3{7.0f, 0.0f, LegendShow::kAhead});
    fixture.show(LegendCue::WornOff, 37);
    REQUIRE(fixture.stopped.size() == 1);
    REQUIRE(fixture.sounds.back() == "S_BLEGWPDN");
}

TEST_CASE("boss-bound legends use the boss offset and stop their sound on wear off",
          "[game][screens][legend]") {
    LegendFixture fixture;
    fixture.load("legend-target");
    fixture.show(LegendCue::Brandished, 39);
    fixture.show(LegendCue::Thrown, 39);
    fixture.bearer.released = true;
    REQUIRE_FALSE(fixture.presentation.update(0.0f, fixture.bearer, fixture.target).landed);
    const auto* burst = fixture.find("LEGENDFX");
    REQUIRE(burst != nullptr);
    REQUIRE(burst->position == fixture.target.position + LegendShow::bossOffsetOf(39));
    REQUIRE(burst->then == "LEGENDFX2");
    REQUIRE(burst->secondsLeft == Approx(LegendShow::burstSecondsOf(39)));
    fixture.show(LegendCue::WornOff, 39);
    REQUIRE(fixture.stopped.size() == 1);
    REQUIRE(fixture.sounds[fixture.sounds.size() - 2] == "S_BLEGWHIT");
    REQUIRE(fixture.sounds.back() == "S_BLEGWPDN");
}

TEST_CASE("missing legend assets or bearer cannot manufacture an impact",
          "[game][screens][legend]") {
    LegendFixture fixture;
    fixture.presentation.show(LegendCue::Brandished, 2, 2, 34, std::nullopt);
    REQUIRE(fixture.presentation.player() == -1);
    fixture.show(LegendCue::Thrown);
    REQUIRE_FALSE(fixture.presentation.update(10.0f, fixture.bearer, fixture.target).landed);
    fixture.show(LegendCue::Brandished);
    fixture.show(LegendCue::Thrown);
    fixture.bearer.released = true;
    REQUIRE_FALSE(fixture.presentation.update(10.0f, fixture.bearer, fixture.target).landed);
    REQUIRE(fixture.effects.count() == 0);
    REQUIRE(fixture.presentation.frozenTexture() == nullptr);
}

TEST_CASE("brandishing a new legend cleans up the previous flight", "[game][screens][legend]") {
    LegendFixture fixture;
    fixture.load("legend-rebrandish");
    fixture.show(LegendCue::Brandished);
    fixture.show(LegendCue::Thrown);
    fixture.bearer.canGesture = false;
    fixture.presentation.update(0.0f, fixture.bearer, fixture.target);
    REQUIRE(fixture.find("LEGENDPRJ") != nullptr);
    fixture.bearer.player = 3;
    fixture.show(LegendCue::Brandished);
    REQUIRE(fixture.presentation.player() == 3);
    REQUIRE(fixture.find("LEGENDPRJ") == nullptr);
    REQUIRE(fixture.find("LEGENDHLD") != nullptr);
    REQUIRE(fixture.stopped == std::vector<SoundHandle>{3});
    REQUIRE(fixture.effects.count() == 1);
}

TEST_CASE("legend audio tries alternate spellings only when the first name is unavailable",
          "[game][screens][legend]") {
    LegendFixture fixture;
    std::vector<std::string> attempted;
    LegendPresentation presentation{
        fixture.effects,
        {fixture.device, fixture.items, fixture.weapons, fixture.shared},
        {[&attempted](std::string_view name) {
             attempted.emplace_back(name);
             return name == "S_JLEGWTHROW" ? kNoSound : SoundHandle{1};
         },
         {}}};
    presentation.show(LegendCue::Brandished, 2, 10, 42, fixture.bearer);
    presentation.show(LegendCue::Thrown, 2, 10, 42, fixture.bearer);
    REQUIRE(attempted == std::vector<std::string>{"S_LEGWPUP", "S_JLEGWTHROW", "S_JEGWTHROW"});
}

TEST_CASE(
    "retail blue relic lightning fans into the bearer instead of facing lengthwise at the camera",
    "[game][screens][legend][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path();
    LegendFixture fixture;
    REQUIRE(fixture.weapons.load(root));
    fixture.bearer.position = Vec3{4, 7, 9};
    fixture.show(LegendCue::Brandished);
    const auto* burst = fixture.find("COMBO_BLU");
    REQUIRE(burst != nullptr);
    const u32 burstId = burst->id;
    const auto slot = fixture.weapons.textures.find("FXCOMBOBLUTEX");
    REQUIRE(slot.has_value());
    const auto frameSlot = fixture.weapons.textures.find("FXCOMBOBLUTEX00");
    REQUIRE(frameSlot.has_value());
    const auto* firstFrame = &fixture.weapons.textures.texture(fixture.device, *frameSlot);
    const auto* secondFrame = &fixture.weapons.textures.texture(fixture.device, *frameSlot + 1);
    const CameraFrame camera = CameraFrame::at({12, 30, -40});
    usize visibleRibbons = 0;
    bool sawFirstFrame = false;
    bool sawSecondFrame = false;

    // The source meshes are eleven XZ strips, with one end at the charge origin
    // and the other 43.0859 units along each authored z. Mode 8 rolls their width
    // toward the camera; it must not replace those eleven separate directions.
    for (s32 tick = 0; tick < 18; ++tick) {
        fixture.effects.update(1.0f / 60.0f);
        burst = fixture.find("COMBO_BLU");
        REQUIRE(burst != nullptr);
        fixture.device.draws.clear();
        fixture.effects.draw(fixture.device, Mat4{1}, {}, &camera);
        usize ribbon = 0;
        for (const auto& draw : fixture.device.draws) {
            if (draw.texture != firstFrame && draw.texture != secondFrame) {
                continue;
            }
            sawFirstFrame |= draw.texture == firstFrame;
            sawSecondFrame |= draw.texture == secondFrame;
            REQUIRE(draw.vertices.size() == 6);
            const usize node = 2 + 2 * ribbon;
            REQUIRE(node < burst->tree->nodes.size());
            CHECK(CameraFrame::facingOf(burst->tree->nodes[node].objectFlags) ==
                  CameraFrame::kFacingTop);
            const Mat4 pose = burst->transform() * burst->pose.matrices()[node];
            const Vec3 origin = (draw.vertices[0].position + draw.vertices[5].position) * 0.5f;
            const Vec3 tip = (draw.vertices[1].position + draw.vertices[2].position) * 0.5f;
            CHECK(glm::distance(origin, fixture.bearer.position) < 0.001f);
            CHECK(glm::distance(tip, Vec3{pose * Vec4{0, 0, 43.0859f, 1}}) < 0.001f);
            const Vec3 normal = glm::cross(draw.vertices[1].position - draw.vertices[0].position,
                                           draw.vertices[2].position - draw.vertices[0].position);
            // Mesh winding is clockwise in the game's left-handed world. Tiny
            // collapsed keys take TopFaceMat's fallback and need not face us.
            if (glm::length(glm::cross(camera.position - origin, Vec3{pose[2]})) >= 0.01f) {
                CHECK(glm::dot(normal, camera.position - origin) <= 0.001f);
                ++visibleRibbons;
            }
            CHECK_FALSE(draw.state.depthWrite);
            ++ribbon;
        }
        CHECK(ribbon == 11);
    }
    CHECK(visibleRibbons > 0);
    CHECK(sawFirstFrame);
    CHECK(sawSecondFrame);
    fixture.effects.update(1.0f);
    CHECK_FALSE(fixture.effects.playing(burstId));
    fixture.presentation.clear();
    CHECK(fixture.effects.count() == 0);
}

TEST_CASE("every costume uses its authored relic charge once at brandishing",
          "[game][screens][legend][assets]") {
    const s32 color = GENERATE(0, 1, 2, 3);
    const s32 boss = GENERATE(34, 41, 42);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path();
    LegendFixture fixture;
    REQUIRE(fixture.weapons.load(root));
    fixture.bearer.color = color;
    CHECK(fixture.effects.count() == 0);
    fixture.show(LegendCue::Brandished, boss);
    const auto* charge = fixture.find(LegendShow::chargeTree(color));
    REQUIRE(charge != nullptr);
    const u32 id = charge->id;
    CHECK(charge->position == fixture.bearer.position);
    CHECK(charge->playbackRate == Approx(LegendShow::kBurstPlaybackRate));
    CHECK_FALSE(charge->repeats);
    for (s32 tick = 0; tick < 90; ++tick) {
        fixture.presentation.update(1.0f / 60, fixture.bearer, fixture.target);
        fixture.effects.update(1.0f / 60);
    }
    CHECK_FALSE(fixture.effects.playing(id));
    CHECK(fixture.find(LegendShow::chargeTree(color)) == nullptr);
    fixture.presentation.clear();
}

} // namespace
