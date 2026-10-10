#include "game/screens/StageResources.h"

#include <algorithm>

#include "engine/core/Log.h"

#include "game/enemies/Bosses.h"
#include "game/enemies/Critters.h"
#include "game/enemies/Enemies.h"

namespace gdl::game {
bool StageResources::preload(const LevelWorld& world, Enemies& enemies, Critters& critters,
                             Bosses& bosses) {
    if (!world.built()) {
        return false;
    }
    const auto* level = world.level();
    const auto roster = level != nullptr ? std::span<const LevelEnemy>{level->enemies}
                                         : std::span<const LevelEnemy>{};
    const auto load = [&](s32 kind) {
        if (!bossNameOf(kind).empty()) {
            return bosses.preload(kind) != nullptr;
        }
        switch (kind) {
        case kGolemEnemyKind: return critters.archiveFor(CombatantKind::Golem) != nullptr;
        case kGargoyleEnemyKind: return critters.archiveFor(CombatantKind::Gargoyle) != nullptr;
        case kGeneralEnemyKind: return critters.archiveFor(CombatantKind::General) != nullptr;
        // IT has no native body but its empty READY tree validates the host's
        // pose. Loading that descriptor does not create or simulate the tagger.
        case kItKind: return enemies.loadKind(kind);
        default:
            return kind >= 0 && (kind < kSwarmKindCount || kind == kDeathKind) &&
                   enemies.loadKind(kind);
        }
    };
    for (const auto& family : roster) {
        if (!load(family.kind)) {
            log::warn("Netplay {}: cannot preload roster family {}", world.ref().name, family.kind);
            return false;
        }
    }
    // Container content may have no standalone instance. Scan native records as
    // well as the roster; don't wait for its first network snapshot to load it.
    for (const auto& info : world.layout().itemInfos()) {
        if (info.type == ItemInfo::kPlacedEnemy) {
            if (const auto kind = enemyKindOf(info.name);
                kind &&
                (!load(levelKindOf(roster, *kind, 1)) || !load(levelKindOf(roster, *kind, 4)))) {
                log::warn("Netplay {}: cannot preload placed family {}", world.ref().name, *kind);
                return false;
            }
        }
    }
    if (level != nullptr && !bossNameOf(level->bossType).empty()) {
        if (bosses.preload(level->bossType) == nullptr) {
            return false;
        }
        // LevelOpponents::finishSummons loads these on Garm's first summon;
        // remote presentation cannot grow its trusted catalog at that point.
        if (level->bossType == 44 && !load(kGarmBroodKind)) {
            return false;
        }
    }
    return true;
}
std::vector<ItemArchive*> StageResources::fixtureArchives(LevelWorld& world, Enemies& enemies,
                                                          Critters& critters, Bosses& bosses) {
    const auto items = world.placedItems().archives();
    std::vector<ItemArchive*> archives(items.begin(), items.end());
    if (world.realmItems().loaded()) {
        archives.push_back(&world.realmItems());
    }
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        if (auto* archive = enemies.archive(kind); archive != nullptr && archive->loaded()) {
            archives.push_back(archive);
        }
    }
    auto stocks = critters.resources();
    for (auto* stock : bosses.resources()) {
        stocks.push_back(stock);
    }
    std::ranges::sort(stocks, {}, [](const auto* stock) { return stock->definition.name; });
    for (auto* stock : stocks) {
        archives.push_back(&stock->archive);
    }
    return archives;
}
} // namespace gdl::game
