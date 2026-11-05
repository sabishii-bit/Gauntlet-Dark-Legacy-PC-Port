#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/world/WorldLighting.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/NameCheats.h"
#include "game/critters/Critters.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelItemArchives.h"
#include "game/level/Traps.h"
#include "game/party/PlayerFigure.h"
#include "game/players/PlayerAnimator.h"

namespace {

using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    return test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
}

usize drawnVertices(const test::FakeRenderDevice& device) {
    usize vertices = 0;
    for (const auto& draw : device.draws) {
        vertices += draw.vertices.size();
    }
    return vertices;
}

/* Every figure the game can put on screen is built from the shipped data and drawn, so that
 * a wrong archive, folder, tree name or flag shows up here rather than as something unseen
 * in play. */

TEST_CASE("every playable class in every colour, and every hidden costume, builds and draws",
          "[census][figure][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    const auto check = [&](const CharacterSave& save, std::string_view what) {
        CAPTURE(what, save.character, save.color, save.name);
        auto figure = PlayerFigure::load(device, root, save, Entrance::Skip);
        REQUIRE(figure != nullptr);
        CHECK(figure->animator().bound());
        figure->animate(1.0f, 2, 1.0f / 30);
        device.draws.clear();
        figure->draw(device, Mat4{1}, Mat4{1}, {}, 1.0f, false);
        CHECK(drawnVertices(device) > 0);
    };
    for (s32 character = 0; character < kClassCount; ++character) {
        for (s32 color = 0; color < kColorCount; ++color) {
            CharacterSave save;
            save.character = character;
            save.color = color;
            check(save, "class");
        }
    }
    for (const HiddenCostume& costume : hiddenCostumes()) {
        CharacterSave save;
        save.name = std::string(costume.name);
        save.character = costume.character;
        save.color = costume.color;
        check(save, "hidden costume");
    }
}

TEST_CASE("every swarm kind's archive binds a body for each tier and draws it",
          "[census][enemies][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        const EnemyKind& info = enemyKind(kind);
        if (info.height < 0 || kind == kItKind || (kind >= kSwarmKindCount && kind != kDeathKind)) {
            continue; // NONE, the bodiless IT, and the great ones, which the critters cover
        }
        CAPTURE(kind, info.name);
        Enemies enemies;
        enemies.open(device, root, nullptr, 4, {}, 1);
        REQUIRE(enemies.loadKind(kind));
        // A tier the archive lacks wears the nearest it has (the garm brood is only GRM3).
        s32 tiers = 0;
        for (s32 tier = 1; tier <= 3; ++tier) {
            tiers += enemies.appearanceTree({kind, tier, 0, false}) != nullptr ? 1 : 0;
        }
        CHECK(tiers >= 1);
        EnemySpawn spawn;
        spawn.kind = kind;
        spawn.placed = true;
        REQUIRE(enemies.spawn(spawn, {}));
        device.draws.clear();
        enemies.draw(device, Mat4{1}, WorldLighting{});
        CHECK(drawnVertices(device) > 0);
    }
}

TEST_CASE("every realm's golem and general costume, and every gargoyle form, builds",
          "[census][critters][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    const auto costumes = [&](std::string_view family) {
        std::vector<char> realms;
        for (const auto& entry : std::filesystem::directory_iterator(root / "MONSTERS" / family)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_directory() && name.size() == 6 && name.starts_with("level")) {
                realms.push_back(static_cast<char>(std::toupper(name.back())));
            }
        }
        REQUIRE_FALSE(realms.empty());
        return realms;
    };
    const auto check = [&](CombatantKind kind, char realm, std::string_view form) {
        CAPTURE(static_cast<s32>(kind), realm, form);
        Critters critters;
        critters.open(device, root, nullptr, {}, realm);
        REQUIRE(critters.archiveFor(kind, form) != nullptr);
        std::optional<s32> id;
        switch (kind) {
        case CombatantKind::Golem: id = critters.spawnGolem(Vec3{0}, 0.0f); break;
        case CombatantKind::General: id = critters.spawnGeneral(Vec3{0}, 0.0f); break;
        default: id = critters.spawnGargoyle(Vec3{0}, 0.0f, form); break;
        }
        REQUIRE(id.has_value());
        critters.update(2, 1.0f / 30, {});
        device.draws.clear();
        critters.draw(device, Mat4{1}, WorldLighting{});
        CHECK(drawnVertices(device) > 0);
    };
    for (const char realm : costumes("GOLEM")) {
        check(CombatantKind::Golem, realm, "");
    }
    for (const char realm : costumes("GENERAL")) {
        check(CombatantKind::General, realm, "");
    }
    for (const std::string_view form : {"GAR_EAGL", "GAR_LION", "GAR_SERP"}) {
        check(CombatantKind::Gargoyle, 'A', form);
    }
}

/** The kinds of placed item the level loader knows how to stand, by the record's type and
 * subtype (SetItem's switch, and the per-subtype branches of the binders). A level naming
 * any other pair would stand something the port does not know. */
bool fixtureKindKnown(s32 type, s32 subtype) {
    switch (type) {
    case ItemInfo::kChoiceList:
    case ItemInfo::kPowerup:
    case ItemInfo::kContainer:
    case ItemInfo::kGenerator:
    case ItemInfo::kPlacedEnemy:
    case ItemInfo::kGate:
    case 11: // transporter
    case 13: // sound, or a music area
        return true;
    case ItemInfo::kTrigger: return subtype >= 20 && subtype <= 31;
    case ItemInfo::kTrap: return subtype >= 0 && subtype <= 5;
    case 9: return subtype == 0 || subtype == 50; // an exit, or the secret realm's
    case 10:
        return subtype == 0 || (subtype >= 40 && subtype <= 45) || subtype == 49 ||
               (subtype >= 51 && subtype <= 53);
    case ItemInfo::kRotator: return subtype == 0 || subtype == 2;
    default: return false;
    }
}

TEST_CASE("every catalogued level's fixtures are of kinds the loader stands, and its traps bind",
          "[census][fixtures][assets]") {
    const auto root = nativeRoot();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    usize visited = 0;
    usize traps = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref);
            CAPTURE(name);
            WorldLayout layout;
            REQUIRE(layout.load(root / ref->directory));
            const auto& infos = layout.itemInfos();
            for (const ItemInstance& instance : layout.itemInstances()) {
                if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
                    continue;
                }
                const ItemInfo& info = infos[static_cast<usize>(instance.info)];
                CAPTURE(info.type, info.subtype, info.name);
                CHECK(fixtureKindKnown(info.type, info.subtype));
            }
            LevelItemArchives items;
            REQUIRE(items.load(root, *ref));
            Traps bound;
            bound.bind(device, layout, items.scope(), nullptr, 1, 1.0f, 1.0f);
            for (usize i = 0; i < bound.size(); ++i) {
                const Traps::Trap& trap = bound.trap(i);
                CAPTURE(trap.name, trap.subtype);
                // Its figure, with the rest, the wind-up, the strike and the recovery.
                CHECK(trap.figure.sequenceCount() >= 3);
                ++traps;
            }
            ++visited;
        }
    }
    CHECK(visited >= 62);
    CHECK(traps > 100);
}

} // namespace
