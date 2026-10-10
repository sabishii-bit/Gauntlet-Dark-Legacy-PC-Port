#include <algorithm>

#include "engine/core/Log.h"

#include "game/enemies/CombatantAssets.h"
#include "game/enemies/Enemies.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/world/PlayerFigure.h"

namespace gdl::game {
namespace {
constexpr u32 kArchiveSlots = 1024;
constexpr u32 kPlayerStart = 4096;
constexpr u32 kPlayerSlots = 8192;
constexpr u32 kPlayerEnd = kPlayerStart + InputCommand::kSeats * kPlayerSlots;
constexpr u32 kEnemySlots = 512;
constexpr u32 kEnemyTrees = 16;
constexpr u32 kEnemyEnd = kPlayerEnd + kEnemyKindCount * kEnemySlots;

bool hasMesh(const TreeInfo& tree) {
    return std::ranges::any_of(tree.nodes, [](const auto& node) {
        return !node.object.empty() || std::ranges::any_of(node.objectFrames, [](const auto& run) {
            return !run.object.empty();
        });
    });
}
} // namespace

bool ProjectileResources::addArchive(u32 firstId, ItemArchive& archive, RenderDevice& device,
                                     std::span<TextureSet* const> lenders) {
    if (!archive.loaded() || archive.trees.size() > kArchiveSlots) {
        return false;
    }
    for (usize index = 0; index < archive.trees.size(); ++index) {
        const auto& tree = archive.trees.tree(static_cast<u32>(index));
        EffectTrees::Effect prototype;
        prototype.archive = &archive;
        prototype.tree = &tree;
        prototype.lenders.assign(lenders.begin(), lenders.end());
        if (effectId(prototype) != 0) {
            continue; // A caster's ordinary throw may live in this same SFX archive.
        }
        // Same sparse WEAP_TW interpretation as PlayerMissiles/EffectTrees: missing
        // static XNWEAP nodes are empty, but animated objects/textures must exist.
        auto geometry = tree;
        if (tree.name.starts_with("WEAP_TW_")) {
            for (auto& node : geometry.nodes) {
                if (!node.object.empty() && !archive.models.find(node.object)) {
                    node.object.clear();
                }
            }
        }
        if (!hasMesh(geometry)) {
            continue; // Particle-only trees are not part of the mesh stream.
        }
        if (!prototype.model.bind(geometry, archive.models, archive.textures, device, lenders) ||
            !addEffect(firstId + static_cast<u32>(index), prototype, device)) {
            log::warn("Netplay: cannot preload projectile tree {}", tree.name);
            return false;
        }
    }
    return true;
}

bool ProjectileResources::addPlayers(RenderDevice& device, ItemArchive& weapons,
                                     const std::array<PlayerFigure*, InputCommand::kSeats>& figures,
                                     std::span<TextureSet* const> lenders) {
    if (!weapons.loaded() ||
        std::ranges::none_of(figures, [](auto* figure) { return figure != nullptr; }) ||
        std::ranges::any_of(m_entries,
                            [](const auto& entry) { return entry.first < kPlayerEnd; })) {
        return false;
    }
    ProjectileResources next = *this;
    const auto streak = weapons.textures.find("WEP_STREAK");
    if (!streak || !next.addStreak(1, weapons.textures.texture(device, *streak)) ||
        !next.addTree(2, weapons, "STARTFX", device) ||
        !next.addArchive(kArchiveSlots, weapons, device, lenders)) {
        return false;
    }
    std::vector<TextureSet*> thrownLenders{&weapons.textures};
    thrownLenders.insert(thrownLenders.end(), lenders.begin(), lenders.end());
    for (usize seat = 0; seat < figures.size(); ++seat) {
        auto* figure = figures[seat];
        if (figure == nullptr) {
            continue;
        }
        const u32 base = kPlayerStart + static_cast<u32>(seat) * kPlayerSlots;
        auto* missile = figure->missileArchive();
        auto* effects = figure->effects();
        if (missile == nullptr || effects == nullptr || !figure->missile().bound() ||
            !next.addModel(base, figure->missile()) ||
            (figure->familiarMissile().bound() &&
             !next.addModel(base + 1, figure->familiarMissile()))) {
            return false;
        }
        // Register only the selected costume's thrown tree, not every body/gear
        // tree in that archive. It uses an extra WEAPONS lender at launch.
        if (!next.addTree(base + 2, *missile, figure->missileTree(), device, thrownLenders) ||
            !next.addArchive(base + kArchiveSlots, *effects, device, lenders) ||
            !next.addArchive(base + 2 * kArchiveSlots, *effects, device, thrownLenders)) {
            return false;
        }
        // Gauntlet bolts borrow the wearer's SFX textures before the scene lenders.
        std::vector<TextureSet*> gauntletLenders{&effects->textures};
        gauntletLenders.insert(gauntletLenders.end(), lenders.begin(), lenders.end());
        constexpr std::array<std::string_view, 2> kGauntlets{"BOSSG_ACID", "BOSSG_ELEC"};
        for (usize hand = 0; hand < kGauntlets.size(); ++hand) {
            if (!next.addTree(base + 3 + static_cast<u32>(hand), weapons, kGauntlets[hand], device,
                              gauntletLenders)) {
                return false;
            }
        }
    }
    *this = std::move(next);
    return true;
}

bool ProjectileResources::addEnemies(RenderDevice& device, Enemies& enemies,
                                     std::span<TextureSet* const> lenders) {
    if (std::ranges::any_of(m_entries, [](const auto& entry) {
            return entry.first >= kPlayerEnd && entry.first < kEnemyEnd;
        })) {
        return false;
    }
    ProjectileResources next = *this;
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        auto* archive = enemies.archive(kind);
        if (archive == nullptr || !archive->loaded()) {
            continue; // IT has no native art; unneeded families are not loaded here.
        }
        if (archive->trees.size() > kEnemySlots - kEnemyTrees) {
            return false;
        }
        const u32 base = kPlayerEnd + static_cast<u32>(kind) * kEnemySlots;
        for (s32 slot = 0; slot < 3; ++slot) {
            if (const auto* model = enemies.projectileModel(kind, slot);
                model != nullptr && !next.addModel(base + static_cast<u32>(slot), *model)) {
                return false;
            }
        }
        if (!next.addArchive(base + kEnemyTrees, *archive, device, lenders)) {
            return false;
        }
    }
    *this = std::move(next);
    return true;
}
bool ProjectileResources::addStage(RenderDevice& device,
                                   const std::array<ItemArchive*, 3>& archives,
                                   std::span<CombatantAssets* const> fighters,
                                   std::span<TextureSet* const> lenders) {
    constexpr u32 kFighterStart = kEnemyEnd + 3 * kArchiveSlots;
    static_assert(kFighterStart + 8 * kArchiveSlots == 65536);
    if (fighters.size() > 8 ||
        std::ranges::any_of(m_entries,
                            [](const auto& entry) { return entry.first >= kEnemyEnd; }) ||
        std::ranges::any_of(fighters, [](const auto* stock) {
            return stock == nullptr || stock->definition.name.empty();
        })) {
        return false;
    }
    ProjectileResources next = *this;
    for (usize i = 0; i < archives.size(); ++i) {
        auto* archive = archives[i];
        if (archive != nullptr && archive->loaded() &&
            !next.addArchive(kEnemyEnd + static_cast<u32>(i) * kArchiveSlots, *archive, device,
                             lenders)) {
            return false;
        }
    }
    std::vector<CombatantAssets*> ordered(fighters.begin(), fighters.end());
    std::ranges::sort(ordered, {}, [](const auto* stock) { return stock->definition.name; });
    for (usize i = 0; i < ordered.size(); ++i) {
        if ((i > 0 && ordered[i - 1]->definition.name == ordered[i]->definition.name) ||
            !next.addArchive(kFighterStart + static_cast<u32>(i) * kArchiveSlots,
                             ordered[i]->archive, device, lenders)) {
            return false;
        }
    }
    *this = std::move(next);
    return true;
}
} // namespace gdl::game
