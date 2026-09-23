#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/LevelWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("the tower loads its geometry, collision, start points and camera markers",
          "[game][world][unpacked]") {
    const std::filesystem::path root = test::unpackedOrSkip("LEVELS/LEVELL1/collision.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE_FALSE(tower.built());
    REQUIRE(tower.load(device, root));
    REQUIRE(tower.built());
    REQUIRE(tower.scene().placedCount() > 2000);
    REQUIRE(tower.collision().loaded());
    REQUIRE(tower.cameraMarkers().size() == 79); // the game camera markers
    REQUIRE(tower.entranceCamera().has_value());

    const WorldLocator* entrance = tower.startPoint(0);
    REQUIRE(entrance != nullptr);
    REQUIRE(entrance->kind == LocatorKind::Start);
    REQUIRE(tower.collision().floorAt(entrance->position, 3.0f, 3.0f).has_value());
    REQUIRE(tower.startPoint(99) == nullptr);
    // Back from the town realm (7) the party stands among its portals, which is the tower's
    // second start marker; from a realm with no ring of its own, at the entrance.
    REQUIRE(LevelWorld::towerMarkerOf(7) == 1);
    REQUIRE(LevelWorld::towerMarkerOf(2) == 2);
    REQUIRE(LevelWorld::towerMarkerOf(13) == 0);
    REQUIRE(LevelWorld::towerMarkerOf(99) == 0);
    REQUIRE(tower.arrivalPoint(7) == tower.startPoint(1));
    REQUIRE(glm::distance(tower.arrivalPoint(7)->position, Vec3{37.8f, -6.3f, -117.5f}) < 0.5f);
    REQUIRE(tower.arrivalPoint(13) == entrance);

    tower.clear();
    REQUIRE_FALSE(tower.built());
    REQUIRE(tower.cameraMarkers().empty());
}

TEST_CASE("the tower takes its light, camera range and sounds from the realm's data",
          "[game][world][unpacked]") {
    test::unpackedOrSkip("LEVELS/LEVELL1/world.json");
    const std::filesystem::path root =
        test::unpackedOrSkip("wdata/TOWER.json").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE(tower.load(device, root));
    REQUIRE(tower.hasLevelData());
    REQUIRE(tower.lighting().ambient.x == Approx(0.8f));
    const Vec3 expected = glm::normalize(Vec3{1.0f, 6.0f, -2.0f});
    REQUIRE(glm::all(glm::epsilonEqual(tower.lighting().direction, expected, 1e-5f)));
    REQUIRE(tower.cameraRange().radiusMin == 24.0f);
    REQUIRE(tower.cameraRange().radiusMax == 32.0f);
    REQUIRE(tower.cameraRange().minPitch == Approx(0.6981317f));
    REQUIRE(tower.audio() != nullptr);
    REQUIRE(tower.audio()->bank == "WIZTOWER");
    REQUIRE(tower.audio()->stream == "tower");
    // The lit level draws its lightmapped batches with their lightmaps.
    tower.draw(device, Mat4{1.0f}, WorldCamera{});
    std::size_t lightmapped = 0;
    for (const auto& draw : device.draws) {
        lightmapped += draw.lightmap() != nullptr ? 1 : 0;
    }
    REQUIRE(lightmapped > 100);
}

TEST_CASE("the tower moves its objects, flickers its torches and lends Sumner his archive",
          "[game][world][unpacked]") {
    test::unpackedOrSkip("LEVELS/LEVELL1/animations.json");
    const std::filesystem::path root = test::unpackedOrSkip("ITEMS/LEVELL/textures.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE(tower.load(device, root));
    REQUIRE(tower.hasItems());
    REQUIRE(tower.items().trees.find("GWIZ").has_value());
    REQUIRE(tower.worldAnimator().size() == 170);
    REQUIRE(tower.textureAnimator().size() >= 50);
    REQUIRE(tower.scene().unitCount() > 500);
    // The torch flames are the item archive's, not white, and change every second frame.
    constexpr std::uint32_t kTorchSlot = 423;
    const Texture* torch = tower.scene().textureOf(kTorchSlot);
    REQUIRE(torch != nullptr);
    REQUIRE(torch != &device.whiteTexture());
    tower.update(2.0f / 30.0f);
    REQUIRE(tower.scene().textureOf(kTorchSlot) != torch);
    // The crossing bridge waits for its trigger; the goldfish swim on.
    REQUIRE(tower.worldAnimator().held(0));
    REQUIRE(tower.worldAnimator().frame(0) == 0.0f);
    std::size_t swimming = 0;
    for (std::size_t i = 0; i < tower.worldAnimator().size(); ++i) {
        if (!tower.worldAnimator().held(i) && tower.worldAnimator().frame(i) == Approx(2.0f)) {
            ++swimming;
        }
    }
    REQUIRE(swimming > 50);
    REQUIRE(tower.triggers().size() == 99);
    REQUIRE(tower.collision().movingObjectCount() > 100);
    // The first realm's force field stands across its gate, solid, its own two triangles
    // placed where the level puts the object.
    std::optional<std::size_t> field;
    for (std::size_t i = 0; i < tower.layout().objects().size(); ++i) {
        if (tower.layout().objects()[i].name == "L1XPTRAPWG") {
            field = i;
        }
    }
    REQUIRE(field.has_value());
    REQUIRE(tower.collision().moving(static_cast<std::int32_t>(*field)));
    REQUIRE(tower.collision().solid(static_cast<std::int32_t>(*field)));
    const Vec3 gate = tower.layout().worldPosition(*field);
    REQUIRE(tower.collision().resolveWalls(gate, 0.75f, gate.y - 1.0f, gate.y + 1.0f) != gate);
    // Every particle marker names one of the level's templates; the braziers burn with the
    // item archive's flame.
    REQUIRE(tower.particles().size() == 206);
    REQUIRE(tower.particles().particleCount() > 100);
    // The crystals Sumner keeps: fifteen orange gems on the floor, for a party of one.
    REQUIRE(tower.placedItems().size() >= 15);
    REQUIRE(tower.placedItems().visibleCount() == 0);
    tower.setPlayerCount(1);
    std::size_t gems = 0;
    for (std::size_t i = 0; i < tower.placedItems().size(); ++i) {
        const PlacedItems::Item& item = tower.placedItems().item(i);
        if (item.name == "GEMORANGE") {
            ++gems;
            REQUIRE(item.visible);
            REQUIRE(item.subtype == ItemInfo::kCrystal);
            REQUIRE(item.instance >= 0);
        }
    }
    REQUIRE(gems == 15);
    REQUIRE(tower.placedItems().visibleCount() >= 15);
    const auto torchTexture = tower.items().textures.find("P_TORCH");
    REQUIRE(torchTexture.has_value());
    bool flames = false;
    for (std::size_t i = 0; i < tower.particles().size(); ++i) {
        flames = flames || tower.particles().textureOf(i) ==
                               &tower.items().textures.texture(device, *torchTexture);
    }
    REQUIRE(flames);
    WorldCamera camera;
    camera.position = Vec3{0.0f, 20.0f, 0.0f};
    tower.draw(device, Mat4{1.0f}, camera);
    REQUIRE(device.draws.size() > 1000);
    // The flames are drawn added onto the frame, after the geometry.
    const Texture* flame = &tower.items().textures.texture(device, *torchTexture);
    bool flamesDrawn = false;
    for (const auto& draw : device.draws) {
        flamesDrawn =
            flamesDrawn || (draw.texture == flame && draw.state.blend == BlendMode::Additive);
    }
    REQUIRE(flamesDrawn);
    // The force field across the first realm's gate: its two triangles added onto the frame
    // with the field texture (animated, so whichever frame the slot shows), at full
    // brightness whichever way it faces.
    constexpr std::uint32_t kFieldSlot = 141;
    const Texture* fieldTexture = tower.scene().textureOf(kFieldSlot);
    REQUIRE(fieldTexture != nullptr);
    bool fieldDrawn = false;
    for (const auto& draw : device.draws) {
        if (draw.texture != fieldTexture || draw.state.blend != BlendMode::Additive ||
            draw.vertices.size() != 6) {
            continue;
        }
        fieldDrawn = true;
        REQUIRE(draw.vertices[0].color.r == 255);
        REQUIRE(draw.vertices[0].color.g == 255);
        REQUIRE(draw.vertices[0].color.b == 255);
    }
    REQUIRE(fieldDrawn);
    tower.clear();
    REQUIRE_FALSE(tower.hasItems());
    REQUIRE(tower.worldAnimator().size() == 0);
    REQUIRE(tower.particles().size() == 0);
    REQUIRE(tower.placedItems().size() == 0);
}

TEST_CASE("the tower reports a missing level without building", "[game][world]") {
    test::FakeRenderDevice device;
    LevelWorld tower;
    REQUIRE_FALSE(tower.load(device, test::scratchDirectory("tower-world-none")));
    REQUIRE_FALSE(tower.built());
    REQUIRE_FALSE(tower.hasLevelData());
    REQUIRE_FALSE(tower.entranceCamera().has_value());
}

} // namespace
