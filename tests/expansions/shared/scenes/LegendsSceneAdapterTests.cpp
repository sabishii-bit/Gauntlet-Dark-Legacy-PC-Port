#include <algorithm>
#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

formats::LegendsMap nativeMap(const std::filesystem::path& directory, LegendsPlatform platform) {
    const bool n64 = platform == LegendsPlatform::N64;
    const auto worlds = readFile(directory / (n64 ? "worlds.wad" : "worlds.rom"));
    const auto models = readFile(directory / (n64 ? "objects.wad" : "objects.rom"));
    auto map = n64 ? formats::LegendsMap::parseN64(worlds, models)
                   : formats::LegendsMap::parseDreamcast(worlds, models);
    if (n64) {
        const auto dynamic = directory / "texdyn.rom";
        const auto dynamicBytes =
            std::filesystem::exists(dynamic) ? readFile(dynamic) : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(models, dynamicBytes)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
    } else {
        map.textures =
            formats::decodeLegendsDreamcastTextures(models, readFile(directory / "textures.rom"));
    }
    return map;
}

TEST_CASE("Native Legends fixtures bind to the shared engine without exported assets",
          "[legends][scene-adapter][assets]") {
    const auto assets = test::dataDirectory().parent_path() / "assets";
    std::filesystem::path directory;
    LegendsPlatform platform = LegendsPlatform::N64;
    SECTION("Nintendo64 mountain") {
        directory = assets / "n64/extracted/levels/mount1";
    }
    SECTION("Dreamcast castle") {
        directory = assets / "dreamcast/extracted/native/levels/levela1";
        platform = LegendsPlatform::Dreamcast;
    }
    const bool n64 = platform == LegendsPlatform::N64;
    if (!std::filesystem::exists(directory)) {
        SKIP("native Legends extraction is not installed");
    }
    auto map = nativeMap(directory, platform);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, platform, directory, map);
    INFO("Native fixture count: " << scene->fixtureCount());
    for (const auto& warning : scene->warnings()) {
        UNSCOPED_INFO(warning);
    }
    REQUIRE(scene->fixtureCount() > 10);
    if (n64) {
        // Shared native texture cycles must initialize the key/food/powerup
        // placeholder slots before TreeModel binds their first frame.
        CHECK(std::ranges::none_of(scene->warnings(), [](const std::string& warning) {
            return warning.starts_with("Unbound fixture: POWERUP:");
        }));
    } else {
        CHECK(scene->textureAnimationCount() > 0);
    }
    const auto frame = CameraFrame::of(WorldCamera{});
    scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Opaque);
    scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Blended);
    REQUIRE_FALSE(device.draws.empty());
    const auto before = device.draws;
    for (s32 tick = 0; tick < 90; ++tick) {
        scene->update(1.0f / 60.0f);
    }
    device.draws.clear();
    scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Opaque);
    scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Blended);
    REQUIRE_FALSE(device.draws.empty());
    bool changed = before.size() != device.draws.size();
    for (usize i = 0; i < std::min(before.size(), device.draws.size()); ++i) {
        const auto& a = before[i];
        const auto& b = device.draws[i];
        changed |= a.texture != b.texture || a.vertices != b.vertices;
    }
    CHECK(changed);
    const auto fixtureCount = scene->fixtureCount();
    scene.reset();
    // Reload after destroying every binding exercises borrowed-archive lifetime.
    // A full console path ending in a separator still selects the same library scope.
    scene = LegendsSceneAdapter::load(device, platform, directory / "", map);
    CHECK(scene->fixtureCount() == fixtureCount);
}

TEST_CASE("Dreamcast tower binds coloured lightmaps additive torches Sumner and its follow camera",
          "[legends][dreamcast-presentation][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelt1";
    if (!std::filesystem::exists(directory))
        SKIP("Dreamcast extraction is not installed");
    auto map = nativeMap(directory, LegendsPlatform::Dreamcast);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(scene->dreamcastCameraConfig());
    CHECK(scene->dreamcastCameraConfig()->radiusMin == 20);
    CHECK(scene->dreamcastCameraConfig()->radiusMax == 26);
    CHECK_FALSE(scene->dreamcastCameraConfig()->authoredBounds);
    REQUIRE(scene->dreamcastCameraMarkers().size() == 7);
    CHECK(std::abs(scene->dreamcastCameraMarkers().front().rotation.y - 2 * kPi) < 0.00001f);
    REQUIRE(map.lightmaps.size() > 200);
    bool coloured = false;
    for (const auto& light : map.lightmaps) {
        REQUIRE_FALSE(light.image.pixels.empty());
        for (usize p = 0; p < light.image.pixels.size(); p += 4) {
            coloured |= light.image.pixels[p] != light.image.pixels[p + 1];
            CHECK(light.image.pixels[p + 3] == 255);
        }
    }
    CHECK(coloured);
    std::optional<s32> wizard;
    for (usize i = 0; i < scene->fixtureCount(); ++i) {
        const auto object =
            scene->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(i));
        if (object && object->name == "WIZARD") {
            wizard = object->id;
            CHECK(object->visible);
            CHECK(object->position == Vec3(0, 0, 16));
        }
    }
    REQUIRE(wizard);
    CHECK(std::ranges::none_of(scene->warnings(),
                               [](const auto& s) { return s == "Unbound fixture: WIZARD"; }));
    const WorldCamera inspectionView;
    const auto inspectWizard = [&] {
        device.draws.clear();
        scene->highlightFixture(device, Mat4{1}, CameraFrame::of(inspectionView), -1, *wizard,
                                SelectionHighlight{inspectionView.position});
        std::vector<Vec3> vertices;
        for (const auto& draw : device.draws)
            for (const auto& vertex : draw.vertices)
                vertices.push_back(Vec3{draw.transform * Vec4{vertex.position, 1}});
        return vertices;
    };
    const auto wizardBefore = inspectWizard();
    REQUIRE_FALSE(wizardBefore.empty());
    scene->update(0.25f);
    CHECK(inspectWizard() != wizardBefore);
    device.draws.clear();

    LegendsLevel level{device, map, std::move(scene), directory, test::dataDirectory()};
    const Vec3 start = map.starts.front().position;
    const std::array subjects{CameraSubject{start, start + Vec3{0, 3, 0}, 3}};
    level.resetCamera(subjects);
    const auto view = level.camera();
    REQUIRE(view);
    REQUIRE(level.cameraAttention());
    CHECK(glm::distance(*level.cameraAttention(), subjects[0].follow) < 0.01f);
    CHECK(view->position.y > start.y);
    CHECK(view->position.z < start.z);
    const auto initial = *view;
    auto moved = subjects;
    moved[0].feet.x += 10;
    moved[0].follow.x += 10;
    for (int i = 0; i < 90; ++i)
        level.followCamera(moved, 1.0f / 30);
    CHECK(glm::distance(initial.position, level.camera()->position) > 1);
    level.draw(device, Mat4{1}, *view, TreeModel::Pass::All, -1);
    CHECK(std::ranges::any_of(device.draws, [](const auto& d) {
        return d.state.lightmapRgb && d.state.lightmap != nullptr &&
               std::ranges::any_of(d.vertices, [](const auto& v) { return v.uv2 != Vec2{0}; });
    }));
    CHECK(std::ranges::any_of(device.draws, [](const auto& d) {
        return d.state.blend == BlendMode::Additive && !d.state.depthWrite && d.state.depthTest;
    }));
}

TEST_CASE("Native Legends fixture presentation binds across the installed map catalog",
          "[legends][scene-adapter-corpus][assets]") {
    const auto assets = test::dataDirectory().parent_path() / "assets";
    for (const auto platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        const auto maps = legendsMaps(platform, assets, {}, {});
        if (maps.empty()) {
            SKIP("native Legends extraction is not installed");
        }
        REQUIRE(maps.size() == (platform == LegendsPlatform::N64 ? 34 : 41));
        for (const auto& entry : maps) {
            DYNAMIC_SECTION((platform == LegendsPlatform::N64 ? "N64 " : "Dreamcast ")
                            << entry.name) {
                auto map = nativeMap(entry.directory, platform);
                test::FakeRenderDevice device;
                auto scene = LegendsSceneAdapter::load(device, platform, entry.directory, map);
                REQUIRE(scene != nullptr);
                if (platform == LegendsPlatform::N64) {
                    for (const auto& warning : scene->warnings()) {
                        INFO(warning);
                        CHECK_FALSE(warning.starts_with("Unbound fixture: POWERUP:"));
                    }
                }
                scene->update(1.0f / 30.0f);
                const auto frame = CameraFrame::of(WorldCamera{});
                scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Opaque);
                scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Blended);
                for (u32 slot = 0; slot < map.textures.size(); ++slot) {
                    // Forces animated and external texture bindings to be sampled too.
                    const auto view = scene->texture(slot);
                    CHECK(std::isfinite(view.offset.x));
                    CHECK(std::isfinite(view.offset.y));
                }
                if (platform == LegendsPlatform::Dreamcast) {
                    // The disc contains levela2 geometry, but Castle's LEVL
                    // roster names A1 twice and never selects A2. Keep that
                    // orphan's generic camera rather than inventing CAMS data.
                    if (entry.name == "levela2") {
                        CHECK_FALSE(scene->dreamcastCameraConfig());
                    } else {
                        REQUIRE(scene->dreamcastCameraConfig());
                    }
                    // Exercise the initial animated pose and decoded lightmaps,
                    // not only the unbound geometry survey for each native map.
                    LegendsPreview preview;
                    preview.open(device, map, std::move(scene));
                    device.draws.clear();
                    preview.drawWorld(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::All, -1);
                    REQUIRE_FALSE(device.draws.empty());
                }
            }
        }
    }
}

TEST_CASE("Tower2 animates only its authored scrolling and borrowed fire texture slots",
          "[legends][tower][scene-adapter][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("native Legends extraction is not installed");
    }
    auto map = nativeMap(directory, LegendsPlatform::N64);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    // The script's locked realm doors carry their own blocking volumes. Walking
    // toward one does not unlock it or push the player backwards.
    const Vec3 beforeDoor{42, 2, -10};
    const Vec3 insideDoor{42, 2, -24};
    const auto stopped = scene->resolveMovement(beforeDoor, insideDoor, 1, 6);
    CHECK(stopped != insideDoor);
    CHECK(glm::distance(stopped, beforeDoor) <= glm::distance(insideDoor, beforeDoor));
    CHECK(scene->resolveMovement(Vec3{0}, Vec3{0, 0, 1}, 1, 6) == Vec3{0, 0, 1});
    usize hidden = 0;
    for (usize i = 0; i < map.objects.size(); ++i) {
        const bool visible = scene->worldObjectVisible(i);
        CHECK(visible == (map.objects[i].name != "CATH_BEAM"));
        if (!visible) {
            ++hidden;
        }
    }
    CHECK(hidden == 1);
    for (const auto& warning : scene->warnings()) {
        UNSCOPED_INFO(warning);
        CHECK_FALSE(warning.starts_with("Unbound fixture: WIZMOVES:"));
        CHECK_FALSE(warning.starts_with("Unbound fixture: PODIUM:"));
        CHECK_FALSE(warning.starts_with("Unbound fixture: DOORS:"));
    }
    const auto slot = [&](std::string_view name) {
        const auto found = std::ranges::find(map.textures, name, &formats::LegendsMapTexture::name);
        REQUIRE(found != map.textures.end());
        return static_cast<u32>(found - map.textures.begin());
    };
    const std::array scrolling{slot("LAVAFLOW"), slot("SNOWFALL"), slot("SWAMPGAS")};
    const std::array fire{slot("FIRE"), slot("TORCHA"), slot("TORCHB"), slot("TORCHC")};
    std::vector<LegendsSceneAdapter::TextureView> before;
    for (u32 i = 0; i < map.textures.size(); ++i) {
        before.push_back(scene->texture(i));
    }
    for (const auto index : fire) {
        REQUIRE(before[index].texture != nullptr);
    }
    scene->update(1.0f / 30.0f);
    for (u32 i = 0; i < before.size(); ++i) {
        if (std::ranges::find(scrolling, i) != scrolling.end()) {
            CHECK(scene->texture(i).offset != before[i].offset);
        } else {
            CHECK(scene->texture(i).offset == before[i].offset);
        }
    }
    std::array<bool, 4> changed{};
    for (s32 frame = 0; frame < 90; ++frame) {
        scene->update(1.0f / 60.0f);
        for (usize i = 0; i < fire.size(); ++i) {
            const auto view = scene->texture(fire[i]);
            REQUIRE(view.texture != nullptr);
            changed[i] |= view.texture != before[fire[i]].texture;
        }
    }
    CHECK(std::ranges::all_of(changed, [](bool value) { return value; }));
}

TEST_CASE("Native Legends pickup collection removes its figure once and returns GDL inventory",
          "[legends][scene-adapter][assets][items]") {
    const auto assets = test::dataDirectory().parent_path() / "assets";
    for (const auto platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        const auto directory = assets / (platform == LegendsPlatform::N64
                                             ? "n64/extracted/levels/mount4"
                                             : "dreamcast/extracted/native/levels/levela1");
        if (!std::filesystem::exists(directory)) {
            SKIP("native Legends extraction is not installed");
        }
        auto map = nativeMap(directory, platform);
        test::FakeRenderDevice device;
        auto scene = LegendsSceneAdapter::load(device, platform, directory, map);
        const auto pickups = scene->pickups();
        INFO(directory.string());
        INFO("Pickups: " << pickups.size());
        for (const auto& pickup : pickups) {
            UNSCOPED_INFO(pickup.name << " kind " << pickup.offer.kind);
        }
        for (const auto& warning : scene->warnings()) {
            UNSCOPED_INFO(warning);
        }
        const auto found = std::ranges::find_if(pickups, [](const auto& pickup) {
            return pickup.offer.kind == static_cast<s32>(ItemKind::Gold);
        });
        REQUIRE(found != pickups.end());
        const usize selected = static_cast<usize>(found - pickups.begin());
        const auto& pickup = pickups[selected];
        CharacterSave save;
        save.name = "TEST";
        PlayerActor actor;
        actor.spawn(0, save, nullptr, pickup.position, 0);
        actor.place(pickup.position - (actor.followPoint() - actor.position()));
        const auto frame = CameraFrame::of(WorldCamera{});
        scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Opaque);
        scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Blended);
        const auto drawsBefore = device.draws.size();
        std::vector<u32> receipts;
        const auto observe = [&](const LegendsPickup& taken, const ItemTaking& result) {
            CHECK(result.took());
            if (taken.nativeIndex == pickup.nativeIndex) {
                CHECK_FALSE(result.card.empty());
                CHECK_FALSE(result.sound.empty());
                CHECK(taken.collected);
            }
            receipts.push_back(taken.nativeIndex);
        };
        REQUIRE_FALSE(scene->collect(actor, 1, false, observe).empty());
        REQUIRE(std::ranges::count(receipts, pickup.nativeIndex) == 1);
        REQUIRE(scene->pickups()[selected].collected);
        CHECK(actor.save().gold >= pickup.offer.amount);
        CHECK(actor.save().progress().relics == save.progress().relics);
        const auto gold = actor.save().gold;
        const auto receiptCount = receipts.size();
        scene->collect(actor, 1, false, observe);
        CHECK(receipts.size() == receiptCount);
        CHECK(actor.save().gold == gold);
        device.draws.clear();
        scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Opaque);
        scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Blended);
        CHECK(device.draws.size() < drawsBefore);
    }
}
TEST_CASE("Native visible activation plates emit their authored sound once on contact",
          "[legends][scene-adapter][native-effects][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/mount1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    auto map = nativeMap(directory, LegendsPlatform::N64);
    const auto definitions = formats::decodeLegendsN64Switches(
        readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
    const auto plate = std::ranges::find_if(definitions.switches, [](const auto& value) {
        return value.supported && value.minimumPlayers <= 1 && !value.hidden() && value.sound != 0;
    });
    REQUIRE(plate != definitions.switches.end());
    const LegendsWorldMotion motion{map, definitions.switches};
    const auto feet = motion.platePosition(plate->nativeIndex);
    REQUIRE(feet);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    scene->takeSounds(); // Discard any independent initially active mechanisms.
    const std::array visitors{LegendsPortalVisitor{0, *feet, true, .1f, 3}};
    scene->touchSwitches(visitors);
    const auto sounds = scene->takeSounds();
    CHECK(std::ranges::count(sounds, plate->sound, &LegendsSoundEvent::token) == 1);
    for (s32 tick = 0; tick < 90; ++tick) {
        scene->update(1.0f / 30);
        scene->touchSwitches(visitors);
    }
    const auto completion = scene->takeSounds();
    REQUIRE(completion.size() == 1);
    CHECK(completion.front().token == 0x1ff); // The lift stops; the plate cue must not replay.
    scene->touchSwitches(visitors);
    CHECK(scene->takeSounds().empty());
}
} // namespace
