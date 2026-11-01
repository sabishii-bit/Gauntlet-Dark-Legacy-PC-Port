#include "engine/core/Error.h"
#include "engine/core/Log.h"

#include "game/enemies/EnemyKinds.h"
#include "game/enemies/LevelOpponents.h"
#include "game/level/ImportedLevel.h"

namespace gdl::game {

void LevelOpponents::openImportedActors() {
    if (!m_resources || !m_resources->world.imported()) {
        return;
    }
    auto& resources = *m_resources;
    const auto actors = resources.world.imported()->importedActors();
    m_importedActors.open(
        resources.device, actors,
        [&](const ImportedActor& actor) -> ItemArchive* {
            if (actor.spawn.kind == kGolemEnemyKind) {
                return m_critters.archiveFor(CombatantKind::Golem);
            }
            if (!m_enemies.loadKind(actor.spawn.kind)) {
                return nullptr;
            }
            if (auto* native =
                    resources.world.imported()->hasEnemyAssets()
                        ? nullptr
                        : resources.world.imported()->importedActorArchive(actor.spawn.kind)) {
                return native;
            }
            return m_enemies.archive(actor.spawn.kind);
        },
        &resources.world.collision());
    for (const auto& warning : m_importedActors.warnings()) {
        log::warn("{}", warning);
    }
    // Critter cues use the costume's native GDL bank. Swarm voices are bound
    // independently below, so borrowing art cannot accidentally mute the golems.
    if (actors.empty() || (resources.world.imported()->hasEnemyAssets() &&
                           std::ranges::none_of(actors, [](const ImportedActor& actor) {
                               return actor.spawn.kind == kGolemEnemyKind;
                           }))) {
        return;
    }
    LevelCatalog catalog;
    if (!catalog.load(resources.root)) {
        return;
    }
    for (const auto& realm : catalog.realms()) {
        if (realm.id == 0) {
            continue;
        }
        WorldData data;
        if (!data.load(resources.root / "wdata" / (realm.file + ".json"))) {
            continue;
        }
        for (const auto& level : data.levels()) {
            if (level.name.empty() || level.name.front() != actors.front().costumeRealm ||
                level.bossType >= 0) {
                continue;
            }
            if (const auto* audio = data.audio(level.audioIndex);
                audio != nullptr && !audio->bank.empty() &&
                resources.audio.borrowBank(resources.root / "audio" / audio->bank)) {
                return;
            }
        }
    }
    log::warn("Imported critters: no counterpart sound bank for {}", actors.front().costumeRealm);
}

void LevelOpponents::updateImportedActors(f32 seconds, std::span<const EnemyView> views,
                                          bool timeStopped) {
    m_importedActors.update(seconds, views, timeStopped, [&](const ImportedActor& actor) {
        if (actor.spawn.kind == kGolemEnemyKind) {
            return m_critters.spawnGolem(actor.spawn.position, actor.yaw, actor.costumeRealm)
                .has_value();
        }
        const auto id = m_enemies.spawn(actor.spawn, views, m_generators.obstacles());
        if (id && actor.spawn.kind == kItKind && !m_resources->world.imported()->hasEnemyAssets()) {
            m_importedActors.bindLive(actor.nativeIndex, *id, m_enemies.instanceOf(*id));
        }
        return id.has_value();
    });
    if (!timeStopped && !views.empty()) {
        if (auto* imported = m_resources->world.imported()) {
            imported->releaseEnemies([&](const EnemySpawn& spawn) {
                return m_enemies.loadKind(spawn.kind) &&
                       m_enemies.spawn(spawn, views, m_generators.obstacles()).has_value();
            });
        }
    }
}

void LevelOpponents::openImportedBoss(const EnemyScales& scales) {
    if (!m_resources || !m_resources->world.imported()) {
        return;
    }
    const auto* definition = m_resources->world.imported()->importedBoss();
    if (!definition) {
        return;
    }
    auto& resources = *m_resources;
    auto nativeAssets =
        resources.world.imported()->loadBossAssets(resources.device, resources.root);
    if (nativeAssets) {
        m_bosses.open(resources.device, resources.root, &resources.world.collision(), scales,
                      definition->counterpartStage.front(), {},
                      resources.world.imported()->bossLegendProfile());
        if (!m_bosses.preload(definition->kind, std::move(nativeAssets))) {
            throw FormatError("Imported boss: native assets could not be installed");
        }
        Vec3 position = definition->position;
        if (const auto floor = resources.world.collision().floorAt(position, 14, 1000))
            position.y = floor->y;
        if (!m_bosses.spawn(definition->kind, position, definition->yaw)) {
            throw FormatError("Imported boss: native combatant could not be created");
        }
        auto* archive = m_bosses.archive();
        m_bossMeter.bind(m_bosses.healthMeters(), archive ? &archive->textures : nullptr);
        if (const auto white = resources.world.powerups().textures.find("AAAWHITE")) {
            m_bosses.setHitFlash(
                &resources.world.powerups().textures.texture(resources.device, *white));
        }
        return;
    }
    LevelCatalog catalog;
    WorldData donor;
    if (!catalog.load(resources.root) ||
        !donor.load(resources.root / "wdata" / (definition->counterpartWorld + ".json"))) {
        throw FormatError("Imported boss: missing counterpart resource catalog");
    }
    const auto ref = catalog.byName(definition->counterpartStage);
    const auto level =
        std::ranges::find(donor.levels(), definition->counterpartStage, &LevelInfo::name);
    const auto* audio = level == donor.levels().end() ? nullptr : donor.audio(level->audioIndex);
    if (!ref || ref->realm != definition->counterpartWorld || !audio ||
        level->bossType != definition->kind || audio->bank != definition->soundBank) {
        throw FormatError("Imported boss: incompatible counterpart resource context");
    }
    // Borrow only the counterpart's asset context. Its world identity, camera,
    // movement limits, loot and campaign rewards must never enter the native map.
    auto textures = std::make_unique<TextureSet>();
    if (!textures->load(resources.root / ref->directory) ||
        !resources.audio.borrowBank(resources.root / "AUDIO" / audio->bank)) {
        throw FormatError("Imported boss: counterpart textures or sounds are unavailable");
    }
    m_bosses.close();
    m_importedBossTextures = std::move(textures);
    const std::array lenders{m_importedBossTextures.get()};
    m_bosses.open(resources.device, resources.root, &resources.world.collision(), scales,
                  definition->counterpartStage.front(), lenders);
    Vec3 position = definition->position;
    // Native Critter creation e0881474 searches fourteen above and one thousand
    // below the authored point. GDL Combatant adds its own model floor offset.
    if (const auto floor = resources.world.collision().floorAt(position, 14, 1000)) {
        position.y = floor->y;
    }
    if (!m_bosses.spawn(definition->kind, position, definition->yaw)) {
        throw FormatError("Imported boss: counterpart combatant could not be created");
    }
    auto* archive = m_bosses.archive();
    m_bossMeter.bind(m_bosses.healthMeters(), archive ? &archive->textures : nullptr);
    if (const auto white = resources.world.powerups().textures.find("AAAWHITE")) {
        m_bosses.setHitFlash(
            &resources.world.powerups().textures.texture(resources.device, *white));
    }
}

} // namespace gdl::game
