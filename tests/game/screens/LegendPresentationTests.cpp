#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

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

} // namespace
