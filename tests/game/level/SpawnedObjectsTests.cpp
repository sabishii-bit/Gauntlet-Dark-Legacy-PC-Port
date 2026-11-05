#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/SpawnedObjects.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path unpackedRoot() {
    return test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
}

TEST_CASE("spawned object snapshots reject another owner and an expired stage binding",
          "[level][spawned-objects][dev-console][local-rewind]") {
    SpawnedObjects objects;
    SpawnedObjects other;
    SpawnedObjects::LocalSnapshot before;
    objects.saveLocal(before);
    REQUIRE(objects.canRestoreLocal(before));
    REQUIRE(objects.restoreLocal(before));
    CHECK_FALSE(other.canRestoreLocal(before));
    CHECK_FALSE(other.restoreLocal(before));
    NativeStateDigest live;
    NativeStateDigest saved;
    REQUIRE(objects.appendNativeDigest(live));
    REQUIRE(objects.appendLocalDigest(saved, before));
    CHECK(live.checkedFinish() == saved.checkedFinish());
    objects.clear();
    CHECK_FALSE(objects.canRestoreLocal(before));
    CHECK_FALSE(objects.restoreLocal(before));
    NativeStateDigest expired;
    CHECK_FALSE(objects.appendLocalDigest(expired, before));
    CHECK_FALSE(expired.checkedFinish());
}

TEST_CASE("an object's tree is the object at the root with its descendants at their offsets",
          "[level][spawned-objects][dev-console][assets]") {
    const auto root = unpackedRoot(); // before any assertion: a skip inside one fails it
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELA1"));
    const auto& objects = layout.objects();
    const auto parent = std::ranges::find_if(objects, [&](const WorldObject& object) {
        return object.child >= 0 && static_cast<usize>(object.child) < objects.size();
    });
    REQUIRE(parent != objects.end());
    const auto index = static_cast<usize>(parent - objects.begin());
    const TreeInfo tree = SpawnedObjects::treeOf(layout, index);
    CHECK(tree.name == parent->name);
    REQUIRE(tree.nodes.size() >= 2);
    CHECK(tree.nodes[0].object == parent->name);
    CHECK(tree.nodes[0].parent == -1);
    CHECK(tree.nodes[0].position == Vec3{0.0f, 0.0f, 0.0f});
    const WorldObject& child = objects[static_cast<usize>(parent->child)];
    CHECK(tree.nodes[1].object == child.name);
    CHECK(tree.nodes[1].parent == 0);
    CHECK(tree.nodes[1].position == child.position);
    // Every node but the root hangs off an earlier one.
    for (usize i = 1; i < tree.nodes.size(); ++i) {
        CHECK(tree.nodes[i].parent >= 0);
        CHECK(static_cast<usize>(tree.nodes[i].parent) < i);
    }
    CHECK(SpawnedObjects::treeOf(layout, objects.size()).nodes.empty());
    CHECK(SpawnedObjects::objectNamed(layout, parent->name) == index);
    std::string lowered = parent->name;
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    CHECK(SpawnedObjects::objectNamed(layout, lowered) == index);
    CHECK_FALSE(SpawnedObjects::objectNamed(layout, "NOTHING_OF_THE_KIND").has_value());
}

TEST_CASE("objects are stood out of their own level's archive, kept once per level, and drawn",
          "[level][spawned-objects][dev-console][assets]") {
    const auto root = unpackedRoot();
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto castle = catalog.byName("A1");
    const auto town = catalog.byName("G1");
    REQUIRE(castle.has_value());
    REQUIRE(town.has_value());
    // An object with a mesh of its own: a group's name has none, and a marker's is never drawn.
    const auto meshed = [&](const LevelRef& level, WorldLayout& layout) {
        REQUIRE(layout.load(root / level.directory));
        ModelSet models;
        REQUIRE(models.load(root / level.directory));
        const auto found = std::ranges::find_if(layout.objects(), [&](const WorldObject& object) {
            return !object.particles() && models.find(object.name).has_value();
        });
        REQUIRE(found != layout.objects().end());
        return found->name;
    };
    WorldLayout townLayout;
    const std::string townObject = meshed(*town, townLayout);
    test::FakeRenderDevice device;
    SpawnedObjects spawned;
    CHECK(spawned.count() == 0);
    CHECK_FALSE(spawned.spawn(device, root, *town, "NOTHING_OF_THE_KIND", Mat4{1.0f}));
    CHECK(spawned.sourceCount() == 1); // the level is loaded for the asking, and kept
    REQUIRE(spawned.spawn(device, root, *town, townObject, Mat4{1.0f}));
    CHECK(spawned.count() == 1);
    CHECK(spawned.sourceCount() == 1);
    device.draws.clear();
    spawned.draw(device, Mat4{1.0f}, WorldLighting{});
    const usize oneDrawn = device.draws.size();
    CHECK(oneDrawn > 0);
    WorldLayout castleLayout;
    const std::string castleObject = meshed(*castle, castleLayout);
    const Mat4 away = glm::translate(Mat4{1.0f}, Vec3{10.0f, 0.0f, 20.0f});
    REQUIRE(spawned.spawn(device, root, *castle, castleObject, away));
    CHECK(spawned.count() == 2);
    CHECK(spawned.sourceCount() == 2);
    device.draws.clear();
    spawned.draw(device, Mat4{1.0f}, WorldLighting{});
    CHECK(device.draws.size() > oneDrawn); // both are drawn
    spawned.clear();
    CHECK(spawned.count() == 0);
    CHECK(spawned.sourceCount() == 0);
}

TEST_CASE("console object births rewind without duplicating or rebinding their models",
          "[level][spawned-objects][dev-console][local-rewind][assets]") {
    const auto root = unpackedRoot();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("A1");
    REQUIRE(level);
    WorldLayout layout;
    ModelSet models;
    REQUIRE(layout.load(root / level->directory));
    REQUIRE(models.load(root / level->directory));
    std::vector<std::string> names;
    for (const auto& object : layout.objects()) {
        if (!object.particles() && models.find(object.name)) {
            names.push_back(object.name);
        }
        if (names.size() == 2) {
            break;
        }
    }
    REQUIRE(names.size() == 2);
    test::FakeRenderDevice device;
    SpawnedObjects objects;
    SpawnedObjects::LocalSnapshot empty;
    SpawnedObjects::LocalSnapshot spawned;
    SpawnedObjects::LocalSnapshot replayed;
    objects.saveLocal(empty);
    const auto placement = glm::translate(Mat4{1}, Vec3{10, 20, 30});
    REQUIRE(objects.spawn(device, root, *level, names[0], placement));
    objects.saveLocal(spawned);
    const auto textures = device.texturesCreated;
    const auto view = objects.view(0);
    REQUIRE(objects.bindingCount() == 1);
    NativeStateDigest original;
    REQUIRE(objects.appendNativeDigest(original));
    REQUIRE(objects.restoreLocal(empty));
    CHECK(objects.count() == 0);
    CHECK(objects.bindingCount() == 1);
    device.draws.clear();
    objects.draw(device, Mat4{1}, {});
    CHECK(device.draws.empty());
    REQUIRE(objects.spawn(device, root, *level, names[0], placement));
    CHECK(objects.count() == 1);
    CHECK(objects.bindingCount() == 1);
    CHECK(device.texturesCreated == textures);
    CHECK(objects.view(0).name == view.name);
    CHECK(objects.view(0).position == view.position);
    objects.saveLocal(replayed);
    CHECK(objects.equalLocal(spawned, replayed));
    NativeStateDigest corrected;
    REQUIRE(objects.appendNativeDigest(corrected));
    CHECK(corrected.checkedFinish() == original.checkedFinish());

    // Retaining a different predicted object cannot leak its cache into native state.
    REQUIRE(objects.restoreLocal(empty));
    REQUIRE(objects.spawn(device, root, *level, names[1], Mat4{1}));
    REQUIRE(objects.restoreLocal(spawned));
    CHECK(objects.count() == 1);
    CHECK(objects.bindingCount() == 2);
    NativeStateDigest afterDifferentPrediction;
    REQUIRE(objects.appendNativeDigest(afterDifferentPrediction));
    CHECK(afterDifferentPrediction.checkedFinish() == original.checkedFinish());
    NativeStateDigest historical;
    REQUIRE(objects.appendLocalDigest(historical, spawned));
    CHECK(historical.checkedFinish() == original.checkedFinish());
    device.draws.clear();
    objects.draw(device, Mat4{1}, {});
    CHECK_FALSE(device.draws.empty());

    auto invalidPlacement = placement;
    invalidPlacement[3].x = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(objects.spawn(device, root, *level, names[0], invalidPlacement));
    CHECK(objects.count() == 1);
    while (objects.count() < SpawnedObjects::kMaxObjects) {
        REQUIRE(objects.spawn(device, root, *level, names[0], placement));
    }
    CHECK_FALSE(objects.spawn(device, root, *level, names[0], placement));
    REQUIRE(objects.restoreLocal(empty));
    CHECK(objects.count() == 0);
    CHECK(objects.bindingCount() == 2);
}

TEST_CASE("spawned object digest bytes stay pinned after console births and what was saved",
          "[level][spawned-objects][dev-console][digest-golden]") {
    const auto root = test::sampleLevel("spawned-objects-golden");
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    test::FakeRenderDevice device;
    SpawnedObjects objects;
    const auto stand = [&](std::string_view name, const Vec3& position) {
        REQUIRE(objects.spawn(device, root, level, name, glm::translate(Mat4{1}, position)));
    };
    stand("WALL", {1, 0, 0});
    stand("WALL", {2, 0, 0});
    SpawnedObjects::LocalSnapshot saved;
    objects.saveLocal(saved);
    stand("WALL", {3, 0, 0});
    stand("WINDOW", {0, 0, 4});
    // SHA-256 of the roster's native digest after four births and of what it saved after two,
    // which netplay peers must hash alike: a literal moves only when what is visited changes on
    // purpose.
    test::checkPinnedDigests("SpawnedObjects", test::liveDigest(objects),
                             test::savedDigest(objects, saved),
                             "0ae7deab4a4d221a2efca7d67f479af1676172d345eef385b8df7bd4d9515275",
                             "7fa78a42a116672aa23c9dda6112556813f8197d008d896eff60a7444f5e775f");
}

} // namespace
