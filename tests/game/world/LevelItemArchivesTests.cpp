#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureBindings.h"
#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"
#include "engine/world/AssetAudit.h"
#include "engine/world/ParticleField.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/LevelItemArchives.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("level item contexts prefer their own art, fall back, and discard stale lenders",
          "[level-asset-context][asset-conformance]") {
    const auto realm = test::sampleLevel("item-context-realm");
    const auto own = test::sampleLevel("item-context-own");
    for (const auto& directory : {realm, own}) {
        writeTextFile(directory / "animations.json", R"({"trees":[{"name":"FIGURE",
          "nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":0,"frameRate":30}]}]})");
    }
    LevelRef ref;
    ref.items = realm.filename().string();
    ref.ownItems = "absent";
    LevelItemArchives items;
    REQUIRE(items.load(realm.parent_path(), ref));
    REQUIRE(items.textureLenders().size() == 1);
    CHECK(items.primary().textures.directory() == realm);
    CHECK_FALSE(items.realm().loaded());
    ref.ownItems = own.filename().string();
    REQUIRE(items.load(realm.parent_path(), ref));
    auto lenders = items.textureLenders();
    REQUIRE(lenders.size() == 2);
    CHECK(lenders[0] == &items.primary().textures);
    CHECK(lenders[1] == &items.realm().textures);
    CHECK(lenders[0]->directory() == own);
    CHECK(lenders[1]->directory() == realm);
    TextureSet empty;
    const auto stone = TextureBindings(empty, lenders).image("STONE");
    REQUIRE(stone);
    CHECK(stone->set == lenders[0]);
    // An unusable own archive must not retain a partly loaded lender.
    writeTextFile(own / "animations.json", "{broken");
    REQUIRE(items.load(realm.parent_path(), ref));
    REQUIRE(items.textureLenders().size() == 1);
    CHECK(items.primary().textures.directory() == realm);
    CHECK_FALSE(items.realm().loaded());
    ref.ownItems = ref.items;
    REQUIRE(items.load(realm.parent_path(), ref));
    CHECK(items.textureLenders().size() == 1);
    ref.ownItems.clear();
    ref.items = "missing";
    CHECK_FALSE(items.load(realm.parent_path(), ref));
    CHECK_FALSE(items.loaded());
    CHECK(items.textureLenders().empty());
    items.clear();
    CHECK(items.textureLenders().empty());
}

TEST_CASE("every catalogued level resolves textures in its actual ordered item context",
          "[assets][asset-conformance][level-asset-context]") {
    const auto root = test::assetOrSkip("WDATA/TOWER.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(catalog.realms().size() == 14);
    CHECK_FALSE(catalog.byName("DEMO1"));
    usize visited = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            DYNAMIC_SECTION("level " << name) {
                const auto ref = catalog.byName(name);
                REQUIRE(ref);
                LevelItemArchives items;
                REQUIRE(items.load(root, *ref));
                const auto lenders = items.textureLenders();
                REQUIRE_FALSE(lenders.empty());
                CHECK(lenders.front() == &items.primary().textures);
                const auto result = auditAssets(root / ref->directory, lenders);
                std::vector<std::string> missing;
                for (const auto& issue : result.issues) {
                    INFO(issue.record << ": " << issue.detail);
                    CHECK(issue.dependency);
                    CHECK(issue.detail == "missing particle template");
                    missing.push_back(issue.record);
                }
                // These native records name absent local templates. WorldPsysActivate
                // (800AB5FC) logs the missing id and skips emitter creation; it does
                // not borrow another level's similarly named effect.
                std::vector<std::string> expected;
                if (name == "I2") {
                    for (int i = 3; i <= 9; ++i) {
                        expected.push_back("world I2PSYSD_EMBERS" + std::to_string(i));
                    }
                } else if (name == "T2") {
                    expected.emplace_back("world T2PSYSA1");
                }
                std::ranges::sort(missing);
                CHECK(missing == expected);
                CHECK(result.dependenciesChecked);
                CHECK(result.passed() == expected.empty());
            }
            ++visited;
        }
    }
    CHECK(visited == 65);
}

TEST_CASE("retail particle markers without local definitions remain inert rather than borrowed",
          "[assets][asset-conformance][level-asset-context]") {
    const auto root = test::assetOrSkip("WDATA/TOWER.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    for (const auto* name : {"I2", "T2", "DEMO1"}) {
        CAPTURE(name);
        const auto directory = std::string(name) == "DEMO1"
                                   ? root / "LEVELS/DEMO1"
                                   : root / catalog.byName(name)->directory;
        WorldLayout layout;
        TextureSet textures;
        REQUIRE(layout.load(directory));
        REQUIRE(textures.load(directory));
        LevelItemArchives items;
        if (const auto ref = catalog.byName(name)) {
            REQUIRE(items.load(root, *ref));
        }
        const auto lenders = items.textureLenders();
        usize absent = 0;
        usize markers = 0;
        for (const auto& object : layout.objects()) {
            if (!object.particles()) {
                continue;
            }
            ++markers;
            const auto at = object.name.find("PSYS");
            REQUIRE(at != std::string::npos);
            REQUIRE(at + 4 < object.name.size());
            absent += layout.findParticleTemplate(object.name[at + 4]) == nullptr ? 1 : 0;
        }
        CHECK(absent == (std::string(name) == "I2" ? 7 : std::string(name) == "T2" ? 1 : 104));
        test::FakeRenderDevice device;
        ParticleField particles;
        particles.bind(layout, textures, device, lenders);
        CHECK(particles.size() == markers - absent);
        particles.step(2.0f);
        CHECK(particles.size() == markers - absent);
    }
}
} // namespace
