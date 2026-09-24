#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
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
    "[game][screens][legend][unpacked]") {
    const auto root = test::unpackedOrSkip("WEAPONS/animations.json").parent_path();
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
          "[game][screens][legend][unpacked]") {
    const s32 color = GENERATE(0, 1, 2, 3);
    const s32 boss = GENERATE(34, 41, 42);
    const auto root = test::unpackedOrSkip("WEAPONS/animations.json").parent_path();
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
