#include "expansions/shared/scenes/LegendsSceneAdapter.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "engine/assets/AssetScope.h"
#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/math/RandomDistribution.h"
#include "engine/platform/Paths.h"

#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/formats/LegendsSceneMotion.h"
#include "expansions/dreamcast/scenes/LegendsInitialPose.h"
#include "expansions/n64/assets/LegendsN64Archive.h"
#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/formats/LegendsN64Tower.h"
#include "expansions/n64/level/LegendsPortalAppearance.h"
#include "expansions/n64/scenes/LegendsScrolls.h"
#include "game/enemies/EnemyKinds.h"

namespace gdl::game {
namespace {

std::vector<u8> optionalFile(const AssetLocator& files, std::string_view name) {
    const auto path = files.find(name);
    return path ? readFile(*path) : std::vector<u8>{};
}

std::vector<u8> requiredFile(const AssetLocator& files, std::string_view name) {
    const auto path = files.find(name);
    if (!path) {
        throw FileError("Legends scene file is missing: " + std::string{name});
    }
    return readFile(*path);
}

TextureSet textureSet(std::vector<formats::LegendsMapTexture> textures) {
    std::vector<TextureSetEntry> entries;
    std::vector<Image> images;
    for (auto& texture : textures) {
        TextureSetEntry entry;
        entry.name = std::move(texture.name);
        entry.width = texture.width != 0 ? texture.width : texture.image.width;
        entry.height = texture.height != 0 ? texture.height : texture.image.height;
        entry.clampU = texture.clampU;
        entry.clampV = texture.clampV;
        entry.flags = texture.hasTransparency ? TextureSetEntry::kHasAlpha : 0;
        if (texture.external) {
            entry.flags |= TextureSetEntry::kExternal;
        }
        entry.noPicture = texture.image.pixels.empty() && !texture.external;
        entries.push_back(std::move(entry));
        images.push_back(std::move(texture.image));
    }
    return TextureSet::fromDecoded(std::move(entries), std::move(images));
}

void normalizeTextureCoordinates(Mesh& mesh, const TextureSet& textures) {
    // Native decoders retain texel coordinates. A shared vertex may belong to
    // materials with different dimensions, so normalize a copy for each slot.
    std::vector<MeshVertex> vertices;
    std::map<std::pair<u32, u32>, u32> remap;
    for (auto& part : mesh.parts) {
        if (part.texture >= textures.size()) {
            throw FormatError("Legends fixture texture index is out of range");
        }
        const auto& entry = textures.entry(part.texture);
        const Vec2 dimensions{std::max(1u, entry.width), std::max(1u, entry.height)};
        for (auto& index : part.indices) {
            const auto key = std::pair{index, part.texture};
            const auto [found, inserted] =
                remap.try_emplace(key, static_cast<u32>(vertices.size()));
            if (inserted) {
                if (index >= mesh.vertices.size()) {
                    throw FormatError("Legends fixture mesh index is out of range");
                }
                auto vertex = mesh.vertices[index];
                vertex.uv /= dimensions;
                vertices.push_back(vertex);
            }
            index = found->second;
        }
    }
    mesh.vertices = std::move(vertices);
}

std::unique_ptr<ItemArchive> dreamcastArchive(const std::filesystem::path& directory) {
    const AssetLocator files{directory};
    const auto models = files.find("objects.rom");
    if (!models) {
        return {};
    }
    const auto bytes = readFile(*models);
    auto objects = formats::decodeDreamcastModelLibrary(bytes);
    auto result = std::make_unique<ItemArchive>();
    result->textures = textureSet(
        formats::decodeLegendsDreamcastTextures(bytes, optionalFile(files, "textures.rom")));
    std::vector<ModelSetEntry> entries;
    std::vector<Mesh> meshes;
    for (auto& object : objects) {
        normalizeTextureCoordinates(object.mesh, result->textures);
        entries.push_back({.name = std::move(object.name)});
        meshes.push_back(std::move(object.mesh));
    }
    result->models = ModelSet::fromDecoded(std::move(entries), std::move(meshes));
    if (const auto animation = files.find("anim.rom")) {
        result->trees =
            AnimationSet::fromDecoded(formats::parseDreamcastAnimation(readFile(*animation)));
    }
    return result;
}

} // namespace

std::unique_ptr<LegendsSceneAdapter>
LegendsSceneAdapter::load(RenderDevice& device, LegendsPlatform platform,
                          const std::filesystem::path& directory, formats::LegendsMap& map,
                          s32 playerCount) {
    if (playerCount < 1 || playerCount > 4) {
        throw std::invalid_argument("Legends scenes require one to four players");
    }
    auto levelDirectory = directory.lexically_normal();
    if (levelDirectory.filename().empty()) {
        levelDirectory = levelDirectory.parent_path();
    }
    auto result = std::make_unique<LegendsSceneAdapter>();
    result->m_platform = platform;
    result->m_playerCount = playerCount;
    AnimationSet worldAnimation;
    if (platform == LegendsPlatform::Dreamcast) {
        const AssetLocator files{levelDirectory};
        const auto motion = formats::LegendsSceneMotion::parseDreamcast(
            requiredFile(files, "worlds.rom"), optionalFile(files, "anim.rom"));
        worldAnimation = AnimationSet::fromDecoded(motion.archive);
        applyDreamcastInitialPose(map, motion);
        result->loadDreamcast(device, levelDirectory);
        const auto models = requiredFile(files, "objects.rom");
        const auto pixels = requiredFile(files, "textures.rom");
        map.lightmaps.clear();
        std::set<u32> lightmaps;
        usize lightmapBytes = 0;
        for (const auto& object : map.objects) {
            for (const u32 record : object.partLightmaps) {
                if (record == 0 || !lightmaps.insert(record).second)
                    continue;
                auto image = formats::decodeDreamcastLightmap(models, pixels, record);
                lightmapBytes += image.pixels.size();
                if (lightmapBytes > 128 * 1024 * 1024)
                    throw FormatError("Dreamcast lightmap budget exceeded");
                map.lightmaps.push_back({record, std::move(image)});
            }
        }
        std::erase(
            map.warnings,
            "Dreamcast lightmaps require their native textures.rom resource at scene binding.");
        if (!motion.worldAnimations.empty()) {
            result->m_warnings.push_back(
                std::format("{} world movement tracks await native trigger/collision integration.",
                            motion.worldAnimations.size()));
        }
        result->m_warnings.insert(result->m_warnings.end(), motion.warnings.begin(),
                                  motion.warnings.end());
    } else {
        result->loadN64(device, levelDirectory, map.collision);
        if (result->m_tower) {
            // Tower e0862fc4 changes this world drawable's material before
            // revealing it. Its foreground light must not write depth over
            // the separately drawn shards; BROKENWINDOW stays behind them.
            for (auto& object : map.objects) {
                if (object.name == "CATH_BEAM") {
                    object.nativeDrawFlags |= 0x10401880U;
                }
            }
        }
        const auto root = levelDirectory.parent_path().parent_path();
        const auto placements = optionalFile(AssetLocator{levelDirectory}, "ipr.rom");
        const auto switches =
            placements.empty()
                ? formats::LegendsN64Switches{}
                : formats::decodeLegendsN64Switches(
                      placements, optionalFile(AssetLocator{root / "items/generic"}, "ptrap.rom"));
        result->bindWorldMotion(map, switches.switches);
        const auto program = optionalFile(AssetLocator{root / "_program"}, "game.bin");
        const auto scale =
            formats::legendsN64TrapScale(program, paths::utf8(levelDirectory.filename()));
        result->m_scenery = LegendsScenery{map.destructibles, scale.value_or(1.0f), map.collision};
        result->updateRiders();
        result->m_warnings.insert(result->m_warnings.end(), switches.warnings.begin(),
                                  switches.warnings.end());
    }
    result->bindWorld(device, map, worldAnimation.textureAnimations());
    if (!result->m_missing.empty()) {
        result->m_warnings.push_back(std::format("{} fixture artwork bindings remain unavailable.",
                                                 result->m_missing.size()));
        constexpr usize kMostDiagnostics = 12;
        for (usize i = 0; i < std::min(kMostDiagnostics, result->m_missing.size()); ++i) {
            result->m_warnings.push_back("Unbound fixture: " + result->m_missing[i]);
        }
    }
    return result;
}

void LegendsSceneAdapter::place(RenderDevice& device, std::string_view name,
                                const ItemInstance& instance, f32 scale, u32 objectFlags) {
    if (name.empty()) {
        return;
    }
    AssetScope scope;
    for (auto& archive : m_archives) {
        scope.add(*archive);
    }
    ItemFigure figure;
    if (!figure.placeStaticFallback(device, scope, name, instance, nullptr, objectFlags,
                                    scope.textureLenders())) {
        if (std::ranges::find(m_missing, name) == m_missing.end()) {
            m_missing.emplace_back(name);
        }
        return;
    }
    figure.placeAt(glm::scale(itemPlacement(instance.position, instance.rotation), Vec3{scale}));
    figure.play(0, true);
    m_fixtures.push_back(std::move(figure));
    m_hiddenFixtures.push_back(false);
    m_heldFixtures.push_back(false);
    m_fixtureOpacity.push_back(1);
}

void LegendsSceneAdapter::loadDreamcast(RenderDevice& device,
                                        const std::filesystem::path& directory) {
    const AssetLocator files{directory};
    const auto scene = formats::LegendsDreamcastScene::parse(requiredFile(files, "worlds.rom"));
    const auto root = directory.parent_path().parent_path();
    const auto metadata = formats::dreamcastLegendsWorldDataFile(paths::utf8(directory.filename()));
    const AssetLocator nativeFiles{root};
    const auto metadataPath = metadata.empty() ? std::nullopt : nativeFiles.find(metadata);
    const auto scope =
        metadataPath ? formats::dreamcastLegendsFixtureScope(readFile(*metadataPath),
                                                             paths::utf8(directory.filename()))
                     : std::nullopt;
    if (!scope) {
        m_warnings.emplace_back("No native level metadata supplies this map's fixture libraries.");
        return;
    }
    m_dreamcastCamera = scope->camera;
    m_dreamcastCameraMarkers = scene.cameraMarkers;
    for (const auto& relative : scope->archives) {
        try {
            if (auto archive = dreamcastArchive(root / relative)) {
                m_archives.push_back(std::move(archive));
            }
        } catch (const FormatError& error) {
            throw FormatError(
                std::format("Legends fixture archive {}: {}", relative, error.what()));
        }
    }
    for (usize index = 0; index < scene.itemInstances.size(); ++index) {
        const auto& instance = scene.itemInstances[index];
        if (instance.info < 0 || !shownToParty(instance.minPlayers, m_playerCount)) {
            continue;
        }
        const auto& info = scene.itemInfos[static_cast<usize>(instance.info)];
        // Trigger pads may have authored artwork; camera and sound markers do not.
        if (info.type != 1 && info.type != 2 && info.type != 3 && info.type != 5 &&
            info.type != 7 && info.type != 8 && info.type != 9 && info.type != 10 &&
            info.type != 11) {
            continue;
        }
        ItemInstance placement;
        placement.position = instance.position;
        placement.rotation = instance.rotation;
        // DC 8c0f29a0/8c0f6a00 use the same facing, additive and depth bits.
        // Other native flags may have simulation meanings; do not copy them.
        constexpr u32 kPresentationFlags = 0x0f8008c0U;
        const usize before = m_fixtures.size();
        place(device, formats::dreamcastLegendsFixtureName(info, instance, *scope), placement, 1,
              (info.objectFlags & kPresentationFlags) | object_flags::kSorted);
        if (m_fixtures.size() != before) {
            std::string diagnostic;
            if (auto pickup = legendsDreamcastPickup(info, instance, static_cast<u32>(index),
                                                     scope->realmType, &diagnostic)) {
                m_pickups.push_back(std::move(*pickup));
                m_pickupFigures.push_back(before);
            }
            if (!diagnostic.empty() &&
                std::ranges::find(m_warnings, diagnostic) == m_warnings.end()) {
                m_warnings.push_back(std::move(diagnostic));
            }
        }
    }
    if (scope->realmType == 13 && toLowerAscii(paths::utf8(directory.filename())) == "levelt1") {
        // Tower wizard creation, 1ST_READ.BIN 8c066640, case2: WIZARD
        // from items/levelT, identity placement at (0,0,16). He is scripted,
        // not an item instance and not the GameCube GWIZ model.
        ItemInstance wizard;
        wizard.position = {0, 0, 16};
        const usize before = m_fixtures.size();
        place(device, "WIZARD", wizard, 1, 0x00881880U);
        if (m_fixtures.size() != before) {
            // Case3 sets drawable transparency to 0x7f via 8c0f71a0;
            // 8c0f29a0 renders alpha=255-transparency. Keep the native
            // additive, non-depth-writing apparition instead of a solid NPC.
            m_fixtureOpacity.back() = 128.0f / 255.0f;
        }
    }
}

ItemArchive* LegendsSceneAdapter::actorArchive(s32 kind) const {
    return kind == kItKind ? m_itArchive : nullptr;
}

void LegendsSceneAdapter::loadN64(RenderDevice& device, const std::filesystem::path& directory,
                                  std::span<const formats::LegendsMapCollisionTriangle> collision) {
    const auto root = directory.parent_path().parent_path();
    const auto program = optionalFile(AssetLocator{root}, "_program/game.bin");
    if (!program.empty()) {
        m_stageUnlocks = LegendsN64Unlocks::decode(program);
        m_gatedPortals = m_stageUnlocks.isTower(paths::utf8(directory.filename()));
    }
    const AssetLocator templates{root / "items/generic"};
    const auto powerups = optionalFile(templates, "ppow.rom");
    const auto decorations = optionalFile(templates, "pdec.rom");
    const auto generators = optionalFile(templates, "pgen.rom");
    const auto traps = optionalFile(templates, "ptrap.rom");
    const auto teleports = optionalFile(templates, "ptel.rom");
    const auto containers = optionalFile(templates, "pcon.rom");
    const auto randoms = optionalFile(templates, "prnd.rom");
    const AssetLocator files{directory};
    for (const auto& scroll :
         formats::decodeLegendsN64TextureScrolls(requiredFile(files, "objects.wad"))) {
        m_nativeScrolls.push_back({scroll, formats::LegendsN64TextureScrollState{scroll}});
    }
    if (const auto settings = files.find("ist.rom")) {
        m_camera = formats::decodeLegendsN64Camera(readFile(*settings),
                                                   paths::utf8(directory.filename()), program);
    }
    const auto placements = optionalFile(files, "ipr.rom");
    const auto fixtures = formats::decodeLegendsN64Fixtures(
        placements, {powerups, decorations, generators, traps, teleports, containers, randoms},
        static_cast<u8>(m_playerCount));
    const auto boss = program.empty()
                          ? std::optional<LegendsBossEncounter>{}
                          : legendsN64Boss(paths::utf8(directory.filename()), program,
                                           optionalFile(AssetLocator{root}, "_program/Boss.bin"));
    const auto rewards = boss ? legendsN64BossRewardFixtures(*boss, powerups)
                              : std::vector<formats::LegendsN64Fixture>{};
    auto archiveFixtures = fixtures;
    archiveFixtures.insert(archiveFixtures.end(), rewards.begin(), rewards.end());
    auto actors = placements.empty()
                      ? LegendsActorRoster{}
                      : decodeLegendsN64Actors(placements, generators, program,
                                               paths::utf8(directory.filename()), collision);
    for (auto& actor : actors.actors) {
        if (shownToParty(actor.minimumPlayers, m_playerCount)) {
            m_actors.push_back(actor);
        }
    }
    m_warnings.insert(m_warnings.end(), actors.warnings.begin(), actors.warnings.end());
    if (std::ranges::any_of(
            m_actors, [](const ImportedActor& actor) { return actor.spawn.kind == kItKind; })) {
        // Native species23's exact e006c0c4/e006a640 route. It is a small
        // billboard with an archive-load texture cycle, not a GC enemy skin.
        try {
            auto archive = loadLegendsN64Archive(root / "it", m_warnings, program);
            if (archive && archive->trees.find("IT:WALK")) {
                m_itArchive = archive.get();
                m_archives.push_back(std::move(archive));
            } else {
                m_warnings.emplace_back("Native It artwork is unavailable: it/IT:WALK.");
            }
        } catch (const FileError& error) {
            m_warnings.push_back("Native It artwork is unavailable: " + std::string{error.what()});
        } catch (const FormatError& error) {
            m_warnings.push_back("Native It artwork is invalid: " + std::string{error.what()});
        }
    }
    const auto portals = formats::decodeLegendsN64Portals(placements, traps, program,
                                                          paths::utf8(directory.filename()));
    const auto switches = placements.empty() ? formats::LegendsN64Switches{}
                                             : formats::decodeLegendsN64Switches(placements, traps);
    const auto gates = placements.empty() ? formats::LegendsN64Gates{}
                                          : formats::decodeLegendsN64Gates(placements, traps);
    const auto hazards = placements.empty() ? formats::LegendsN64Traps{}
                                            : formats::decodeLegendsN64Traps(placements, traps);
    const auto trapScale = formats::legendsN64TrapScale(program, paths::utf8(directory.filename()));
    m_warnings.insert(m_warnings.end(), hazards.warnings.begin(), hazards.warnings.end());
    if (!hazards.traps.empty() && !trapScale) {
        m_warnings.emplace_back("Native trap stage damage scale is unavailable.");
    }
    const auto obelisks = placements.empty() ? formats::LegendsN64Obelisks{}
                                             : formats::decodeLegendsN64Obelisks(placements, traps);
    m_warnings.insert(m_warnings.end(), gates.warnings.begin(), gates.warnings.end());
    m_warnings.insert(m_warnings.end(), obelisks.warnings.begin(), obelisks.warnings.end());
    bindN64Selectors(powerups, randoms, fixtures);
    if (std::ranges::any_of(fixtures, [&](const auto& fixture) {
            return decodeLegendsN64Explosive(fixture, traps).has_value();
        })) {
        if (auto archive = loadLegendsN64Archive(root / "weapons", m_warnings, program)) {
            m_archives.push_back(std::move(archive));
        }
    }
    for (const auto& relative : legendsN64GeneratorArchives(paths::utf8(directory.filename()))) {
        if (auto archive = loadLegendsN64Archive(root / relative, m_warnings, program)) {
            m_archives.push_back(std::move(archive));
        }
    }
    for (const auto& relative : formats::legendsN64FixtureArchives(
             paths::utf8(directory.filename()), archiveFixtures, static_cast<u8>(m_playerCount))) {
        try {
            if (auto archive = loadLegendsN64Archive(root / relative, m_warnings, program)) {
                m_archives.push_back(std::move(archive));
            }
        } catch (const FormatError& error) {
            throw FormatError(
                std::format("Legends fixture archive {}: {}", relative, error.what()));
        }
    }
    if (boss) {
        if (!boss->combat.arenaRocks.empty()) {
            // e006b110 creates SAFEROCK after loading the native stage. Its
            // sequences and model slots belong to that stage's own archives,
            // not the generic item banks used by ordinary placed fixtures.
            if (auto archive = loadLegendsN64Archive(directory, m_warnings, program)) {
                m_archives.push_back(std::move(archive));
            }
        }
        bindN64BossRewards(device, *boss, rewards);
        bindN64Arena(device, boss->combat);
    }
    for (const auto& fixture : fixtures) {
        if (!shownToParty(fixture.minimumPlayers, m_playerCount) ||
            fixture.actorInsteadOfGenerator || fixture.sequence.empty()) {
            continue;
        }
        ItemInstance placement;
        placement.position = formats::legendsN64FixturePosition(fixture, collision);
        placement.rotation.y = fixture.facingRadians;
        const usize before = m_fixtures.size();
        const usize firstPickup = m_pickups.size();
        const usize firstContainer = m_containers.size();
        const usize firstGate = m_gates.size();
        auto tree = formats::legendsN64TreeName(fixture.group, fixture.sequence);
        if (const auto generator = legendsN64Generator(
                fixture, generators, paths::utf8(directory.filename()), collision)) {
            // e0067818 replaces generic PGEn templates with the stage species
            // before creating their tree. For example castle4 places GGR*, but
            // loads knight GKN* artwork; requiring the original tree first
            // would discard a valid generator before its gameplay can bind.
            tree = generator->sequences[static_cast<usize>(generator->gameplay.tier)];
        }
        place(device, tree, placement, fixture.scale);
        if (m_fixtures.size() != before) {
            auto& figure = m_fixtures.back();
            if (const auto* initial = figure.sequenceInfo(0)) {
                // N64 sequence headers decide whether an idle/effect repeats or
                // holds its end; do not loop every authored one-shot by default.
                figure.play(0, initial->repeats);
            }
            std::string diagnostic;
            if (!m_lootSelectors.find(fixture.nativeIndex, false) ||
                fixture.kind != formats::LegendsN64FixtureKind::Powerup) {
                bindN64Death(fixture, before, collision);
            }
            if (auto pickup = legendsN64Pickup(fixture, &diagnostic, m_playerCount)) {
                pickup->position = placement.position;
                m_pickups.push_back(std::move(*pickup));
                m_pickupFigures.push_back(before);
            }
            if (!LegendsDeath::decode(fixture, m_playerCount) && !diagnostic.empty() &&
                std::ranges::find(m_warnings, diagnostic) == m_warnings.end()) {
                m_warnings.push_back(std::move(diagnostic));
            }
            if (fixture.kind == formats::LegendsN64FixtureKind::Trap) {
                const auto trap = std::ranges::find(hazards.traps, fixture.nativeIndex,
                                                    &formats::LegendsN64Trap::nativeIndex);
                if (trap != hazards.traps.end() && trapScale) {
                    const auto projected = formats::legendsN64FixturePlacement(fixture, collision);
                    bindN64Trap(device, *trap, before, *trapScale, projected.floorObject);
                }
                const auto gate = std::ranges::find(gates.gates, fixture.nativeIndex,
                                                    &formats::LegendsN64Gate::nativeIndex);
                if (gate != gates.gates.end()) {
                    bindN64Gate(device, *gate, before);
                }
                const auto obelisk = std::ranges::find(obelisks.obelisks, fixture.nativeIndex,
                                                       &formats::LegendsN64Obelisk::nativeIndex);
                if (obelisk != obelisks.obelisks.end()) {
                    bindN64Obelisk(device, *obelisk, before);
                }
                const auto plate = std::ranges::find(switches.switches, fixture.nativeIndex,
                                                     &formats::LegendsN64Switch::nativeIndex);
                if (plate != switches.switches.end()) {
                    bindN64Switch(device, *plate, before);
                }
                const auto portal = std::ranges::find(portals, fixture.nativeIndex,
                                                      &formats::LegendsN64Portal::nativeIndex);
                if (portal != portals.end()) {
                    auto projected = *portal;
                    projected.position = placement.position;
                    bindPortal(device, std::move(projected), before);
                }
            }
            if (auto container = decodeLegendsN64Container(fixture, containers, powerups)) {
                bindN64Container(device, std::move(*container), before);
            }
            if (auto barrel = decodeLegendsN64Explosive(fixture, traps)) {
                bindN64Container(device, std::move(*barrel), before);
            }
            bindN64Generator(device, fixture, generators, paths::utf8(directory.filename()),
                             collision, before);
            bindN64FloorSelector(device, fixture, before);
            bindN64Rider(fixture, collision, before, firstPickup, firstContainer, firstGate);
        }
    }
    if (!m_obelisks.empty()) {
        bindN64ObeliskEffects(device, root);
    }
    const auto bindScroll = [&](LegendsPickup& pickup) {
        if (pickup.offer.kind == static_cast<s32>(ItemKind::Scroll)) {
            pickup.scrollPages = loadLegendsN64Scroll(directory, program, pickup.offer.amount);
            if (pickup.scrollPages.empty()) {
                m_warnings.emplace_back("Native scroll text is unavailable for this placement.");
            }
        }
    };
    for (auto& pickup : m_pickups) {
        bindScroll(pickup);
    }
    for (auto& selector : m_floorSelectors) {
        for (auto& outcome : selector.outcomes) {
            if (outcome.pickup) {
                bindScroll(*outcome.pickup);
            }
        }
    }
    if (m_camera && m_camera->tower) {
        loadN64Tower(device, root, program);
    }
}

void LegendsSceneAdapter::bindPortal(RenderDevice& device, formats::LegendsN64Portal portal,
                                     usize idleFigure) {
    if ((portal.placementFlags & 8U) != 0) {
        // Native activation/script-hidden exits cannot be enabled by proximity alone.
        m_hiddenFixtures[idleFigure] = true;
        return;
    }
    if (portal.map.empty()) {
        const std::string warning = "A native exit needs campaign-return destination context.";
        if (std::ranges::find(m_warnings, warning) == m_warnings.end()) {
            m_warnings.push_back(warning);
        }
        return;
    }
    AssetScope scope;
    for (auto& archive : m_archives) {
        scope.add(*archive);
    }
    if (!scope.findTree(formats::legendsN64TreeName("TRAP", portal.active))) {
        m_warnings.push_back("Native exit active artwork is unavailable: " + portal.active);
        return;
    }
    ItemInstance instance;
    instance.position = portal.position;
    instance.rotation.y = portal.yaw;
    PortalPresentation binding;
    binding.figures.fill(idleFigure);
    binding.active = idleFigure;
    if (const auto* tree = m_fixtures[idleFigure].presentation().tree) {
        binding.lockedMeshes = legendsLockedPortalMeshes(*tree);
    }
    // Tower exits are closed until an actual attending party supplies its
    // independent N64 history. An ordinary stage's return exit is not gated.
    binding.enabled = !m_gatedPortals;
    const std::array<std::string_view, 4> sequences{portal.idle, portal.starting, portal.active,
                                                    portal.closing};
    for (usize i = 1; i < sequences.size(); ++i) {
        const auto name = formats::legendsN64TreeName("TRAP", sequences[i]);
        if (sequences[i].empty() || !scope.findTree(name)) {
            continue;
        }
        const usize index = m_fixtures.size();
        place(device, name, instance, portal.scale);
        if (m_fixtures.size() != index) {
            binding.figures[i] = index;
            m_hiddenFixtures[index] = true;
        }
    }
    if (binding.figures[2] == idleFigure) {
        return;
    }
    const bool starting = binding.figures[1] != idleFigure;
    const bool closing = binding.figures[3] != idleFigure;
    const auto* initial = m_fixtures[binding.figures[starting ? 1 : 2]].sequenceInfo(0);
    binding.motion =
        LegendsPortalMotion(initial ? static_cast<u32>(initial->frames) : 0, starting, closing);
    m_portals.push_back(std::move(portal));
    m_portalPresentation.push_back(std::move(binding));
}

void LegendsSceneAdapter::selectPortal(usize index) {
    auto& portal = m_portalPresentation[index];
    if (portal.generation == portal.motion.generation()) {
        return;
    }
    portal.generation = portal.motion.generation();
    usize variant = 0;
    switch (portal.motion.phase()) {
    case LegendsPortalMotion::Phase::Idle: variant = 0; break;
    case LegendsPortalMotion::Phase::Starting: variant = 1; break;
    case LegendsPortalMotion::Phase::Active:
    case LegendsPortalMotion::Phase::Departed: variant = 2; break;
    case LegendsPortalMotion::Phase::Closing: variant = 3; break;
    }
    if (portal.active == portal.figures[variant]) {
        // Native e0068118 only changes the player's state when readiness
        // completes. The already-running EXNA ring must not restart then.
        return;
    }
    m_hiddenFixtures[portal.active] = true;
    portal.active = portal.figures[variant];
    m_hiddenFixtures[portal.active] = false;
    auto& figure = m_fixtures[portal.active];
    const auto* info = figure.sequenceInfo(0);
    const bool transition = variant == 1 || variant == 3;
    figure.play(0, !transition && info != nullptr && info->repeats);
    figure.capturePresentation();
}

void LegendsSceneAdapter::updatePortals() {
    for (usize i = 0; i < m_portalPresentation.size(); ++i) {
        auto& portal = m_portalPresentation[i];
        if (portal.enabled && m_fixtures[portal.active].finished()) {
            portal.motion.finishPresentation();
            selectPortal(i);
        }
    }
}

void LegendsSceneAdapter::cancelPortalTravel() {
    m_portalDeparture = false;
    m_waitForPortalExit = true;
    for (usize i = 0; i < m_portalPresentation.size(); ++i) {
        m_portalPresentation[i].motion.reset();
        selectPortal(i);
    }
    m_towerPortals = {LegendsTowerPortalMotion{LegendsTowerPortalKind::Shards},
                      LegendsTowerPortalMotion{LegendsTowerPortalKind::Runestones}};
    updateTowerPortals();
}

std::optional<LegendsTowerPortalKind> LegendsSceneAdapter::towerDeparture() const {
    for (const auto& portal : m_towerPortals) {
        if (portal.departed()) {
            return portal.kind();
        }
    }
    return {};
}

void LegendsSceneAdapter::updateTowerPortals() {
    const auto progress = LegendsTowerProgress::of(m_partyProgress);
    if (m_towerShardPortal) {
        // e08641c4 reveals the cathedral's exit with its contact charge,
        // not simply because the four installed shards enable CATH_BEAM.
        m_hiddenFixtures[*m_towerShardPortal] =
            !progress.cathedralBeam || m_towerPortals[0].charge() <= 0;
    }
    if (m_towerRunePortal) {
        const auto& portal = m_towerPortals[1];
        m_hiddenFixtures[*m_towerRunePortal] = !progress.runestonePortal || !portal.fireCreated();
        m_fixtures[*m_towerRunePortal].placeAt(glm::translate(
            Mat4{1}, portal.definition().position + Vec3{0, portal.fireHeight(), 0}));
    }
}

std::vector<PointLight> LegendsSceneAdapter::towerPortalLights() const {
    std::vector<PointLight> result;
    if (!m_camera || !m_camera->tower)
        return result;
    for (usize i = 0; i < m_towerPortals.size(); ++i) {
        const auto& portal = m_towerPortals[i];
        const f32 charge = portal.charge();
        if (charge <= 0)
            continue;
        // e0862fc4/e0861630 create radius10 lights. e08641c4/e0864914
        // update RGB, with a warm 1:1/6:1/8 ratio for the rune fire.
        const Vec3 color = i == 0 ? Vec3{charge} : Vec3{charge, charge / 6, charge / 8};
        result.push_back({portal.definition().position, color, 10, 2});
    }
    return result;
}

std::optional<std::string>
LegendsSceneAdapter::tickPortals(std::span<const LegendsPortalVisitor> players, f32 seconds) {
    if (m_portalDeparture) {
        return std::nullopt;
    }
    u8 seen = 0;
    u8 living = 0;
    for (const auto& player : players) {
        if (player.player >= 4 || (seen & (1U << player.player)) != 0) {
            return std::nullopt;
        }
        const auto bit = static_cast<u8>(1U << player.player);
        seen |= bit;
        if (player.alive) {
            living |= bit;
        }
    }
    if (m_waitForPortalExit) {
        // A failed load leaves the party on the existing exit. Require them to
        // leave its actual native contact volumes before charging a fresh try;
        // otherwise a missing file would be retried every frame indefinitely.
        for (const auto& player : players) {
            if (!player.alive)
                continue;
            if (!std::isfinite(player.position.x) || !std::isfinite(player.position.y) ||
                !std::isfinite(player.position.z))
                return std::nullopt;
            for (const auto& portal : m_portals) {
                if (touchesLegendsPortal(portal, player))
                    return std::nullopt;
            }
            if (m_camera && m_camera->tower) {
                for (const auto& portal : m_towerPortals) {
                    const auto& definition = portal.definition();
                    const f64 x = static_cast<f64>(player.position.x) - definition.position.x;
                    const f64 z = static_cast<f64>(player.position.z) - definition.position.z;
                    if (x * x + z * z < static_cast<f64>(definition.radius) * definition.radius)
                        return std::nullopt;
                }
            }
        }
        if (living == 0)
            return std::nullopt;
        m_waitForPortalExit = false;
        return std::nullopt;
    }
    if (m_camera && m_camera->tower) {
        const auto progress = LegendsTowerProgress::of(m_partyProgress);
        const std::array enabled{progress.cathedralBeam, progress.runestonePortal};
        for (usize i = 0; i < m_towerPortals.size(); ++i) {
            const bool fireCreated = m_towerPortals[i].fireCreated();
            const bool ready = m_towerPortals[i].advance(seconds, players, enabled[i]);
            if (i == 1 && !fireCreated && m_towerPortals[i].fireCreated() && m_towerRunePortal) {
                // The native tree is created on first contact, not when its
                // bank is loaded. A canceled transition may create it anew;
                // ordinary leaving/re-entry keeps the existing animation.
                auto& figure = m_fixtures[*m_towerRunePortal];
                const auto* sequence = figure.sequenceInfo(0);
                figure.play(0, sequence != nullptr && sequence->repeats);
            }
            if (ready) {
                updateTowerPortals();
                m_portalDeparture = true;
                return std::string{m_towerPortals[i].definition().map};
            }
        }
        updateTowerPortals();
    }
    for (usize i = 0; i < m_portals.size(); ++i) {
        u8 occupied = 0;
        for (const auto& player : players) {
            if (touchesLegendsPortal(m_portals[i], player)) {
                occupied |= static_cast<u8>(1U << player.player);
            }
        }
        const bool ready = m_portalPresentation[i].motion.advance(seconds, occupied, living,
                                                                  m_portalPresentation[i].enabled);
        selectPortal(i);
        if (ready) {
            m_portalDeparture = true;
            return m_portals[i].map;
        }
    }
    return std::nullopt;
}

void LegendsSceneAdapter::loadN64Tower(RenderDevice& device, const std::filesystem::path& root,
                                       std::span<const u8> program) {
    const auto native =
        formats::decodeLegendsN64Tower(requiredFile(AssetLocator{root}, "_program/Tower.bin"));
    m_initiallyHiddenWorldObjects = native.initiallyHiddenWorldObjects;
    m_towerResidentVolumes.assign(native.residentVolumes.begin(), native.residentVolumes.end());
    for (const auto& figure : {native.sumner, native.podium}) {
        if (auto archive = loadLegendsN64Archive(root / figure.models, m_warnings, program,
                                                 root / figure.animations)) {
            m_archives.push_back(std::move(archive));
        }
    }
    TowerPresentation tower;
    const auto variants = [&](const formats::LegendsN64TowerFigure& source,
                              std::initializer_list<std::string_view> sequences,
                              std::vector<TowerPresentation::Variant>& result) {
        ItemInstance instance;
        instance.position = source.position;
        instance.rotation.y = source.yaw;
        for (const auto sequence : sequences) {
            const usize slot = m_fixtures.size();
            place(device, formats::legendsN64TreeName(source.group, sequence), instance);
            if (m_fixtures.size() != slot) {
                m_fixtures.back().play(0, false);
                m_hiddenFixtures[slot] = true;
                result.push_back({std::string{sequence}, slot});
            }
        }
    };
    // Prebind every idle state during load: choosing one later cannot upload a
    // texture in the middle of a rendered frame. These are script-created trees,
    // absent from IPR. Staff remains hidden outside ceremonies, as in retail.
    variants(native.sumner, {"LKDN", "READ", "PAGE", "CNJ1", "CNJ2", "CNJ3"}, tower.sumner);
    variants(native.podium, {"REDY", "FLIP"}, tower.podium);
    for (const auto& door : native.doors) {
        const usize slot = m_fixtures.size();
        place(device, formats::legendsN64TreeName("DOORS", door.openingSequence), {});
        if (m_fixtures.size() != slot) {
            m_fixtures.back().play(0, false);
            m_heldFixtures[slot] = true;
            const usize open = m_fixtures.size();
            place(device, formats::legendsN64TreeName("DOORS", door.openSequence), {});
            if (m_fixtures.size() != open) {
                m_hiddenFixtures[open] = true;
                m_towerDoors.push_back({door, slot, open, LegendsTowerDoorMotion{}});
            } else {
                m_towerDoors.push_back({door, slot, slot, LegendsTowerDoorMotion{}});
            }
        }
    }
    // These are installed static models, not floor pickup trees. Bind the
    // native archives up front, including currently unowned pieces, so later
    // progress changes do not create GPU resources during a frame.
    std::vector<std::string> relicArchives;
    const auto relics = [&](const auto& definitions, auto& bindings) {
        for (usize i = 0; i < definitions.size(); ++i) {
            const auto& relic = definitions[i];
            if (std::ranges::find(relicArchives, relic.archive) == relicArchives.end()) {
                if (auto archive =
                        loadLegendsN64Archive(root / relic.archive, m_warnings, program)) {
                    m_archives.push_back(std::move(archive));
                }
                relicArchives.push_back(relic.archive);
            }
            const usize slot = m_fixtures.size();
            // Only the independently verified depth/sort flags share encoding
            // with GDL. Native 0x10000000 is not a GDL facing mode.
            place(device, relic.model, {}, 1,
                  relic.nativeDrawFlags & (object_flags::kNoDepthTest |
                                           object_flags::kNoDepthWrite | object_flags::kSorted));
            if (m_fixtures.size() != slot) {
                m_fixtures[slot].placeAt(relic.placement);
                m_hiddenFixtures[slot] = true;
                bindings[i] = slot;
            }
        }
    };
    relics(native.runestones, m_towerRunestones);
    relics(native.shards, m_towerShards);
    ItemInstance portal;
    portal.position = native.shardPortal.position;
    portal.rotation.y = native.shardPortal.yaw;
    const usize portalFigure = m_fixtures.size();
    place(device,
          formats::legendsN64TreeName(native.shardPortal.group, native.shardPortal.sequence),
          portal, native.shardPortalScale);
    if (m_fixtures.size() != portalFigure) {
        m_towerShardPortal = portalFigure;
        m_hiddenFixtures[portalFigure] = true;
        if (const auto* tree = m_fixtures[portalFigure].tree()) {
            // e0863fb4 uses the same CENTER-own / XCOANI-subtree suppression
            // as the native locked-exit presentation, without locking admission.
            m_towerPortalHiddenMeshes = legendsLockedPortalMeshes(*tree);
        }
    }
    // e0861630 loads this bank; e0864914 creates its sequence-zero tree
    // when any visitor approaches the completed runestone circle. Prebind
    // it here so first contact needs no GPU allocation during gameplay.
    if (auto archive = loadLegendsN64Archive(root / "bosseffect/skodeath", m_warnings, program)) {
        // TEXR is empty, but the fifteen local BOSSES_FIRE frames live in
        // this bank's texdyn.rom. The archive loader resolves those dynamic
        // pixels and seeds its animated placeholder; no boss bank is needed.
        m_archives.push_back(std::move(archive));
        ItemInstance fire;
        fire.position = m_towerPortals[1].definition().position + Vec3{0, -40, 0};
        const usize figure = m_fixtures.size();
        place(device, formats::legendsN64TreeName("BOSSDEATHFX", {}), fire);
        if (m_fixtures.size() != figure) {
            m_towerRunePortal = figure;
            m_hiddenFixtures[figure] = true;
            const auto* sequence = m_fixtures[figure].sequenceInfo(0);
            m_fixtures[figure].play(0, sequence != nullptr && sequence->repeats);
        }
    }
    const auto activate = [&](std::span<const TowerPresentation::Variant> variants,
                              std::string_view sequence, usize& active) {
        const auto found =
            std::ranges::find(variants, sequence, &TowerPresentation::Variant::sequence);
        if (found == variants.end()) {
            return false;
        }
        active = found->figure;
        m_hiddenFixtures[active] = false;
        return true;
    };
    const bool sumner = activate(tower.sumner, native.sumner.sequence, tower.activeSumner);
    const bool podium = activate(tower.podium, native.podium.sequence, tower.activePodium);
    if (sumner && podium) {
        m_tower = std::move(tower);
    } else {
        m_warnings.emplace_back("Tower scripted idle artwork is incomplete.");
    }
}

void LegendsSceneAdapter::updateTower() {
    for (auto& door : m_towerDoors) {
        const auto before = door.motion.state();
        door.motion.update(m_fixtures[door.openingFigure].finished());
        if (before == LegendsTowerDoorMotion::State::Opening &&
            door.motion.state() == LegendsTowerDoorMotion::State::Open &&
            door.openFigure != door.openingFigure) {
            m_hiddenFixtures[door.openingFigure] = true;
            m_hiddenFixtures[door.openFigure] = false;
        }
    }
    if (!m_tower) {
        return;
    }
    auto& tower = *m_tower;
    const auto select = [&](std::span<const TowerPresentation::Variant> variants,
                            std::string_view sequence, usize& active) {
        const auto found =
            std::ranges::find(variants, sequence, &TowerPresentation::Variant::sequence);
        if (found != variants.end()) {
            m_hiddenFixtures[active] = true;
            active = found->figure;
            m_hiddenFixtures[active] = false;
            m_fixtures[active].play(0, false);
            m_fixtures[active].capturePresentation();
        }
    };
    if (m_fixtures[tower.activePodium].finished() &&
        tower.activePodium != tower.podium.front().figure) {
        select(tower.podium, "REDY", tower.activePodium);
    }
    if (m_fixtures[tower.activeSumner].finished()) {
        const auto random =
            static_cast<u16>(*random_distribution::boundedIndex(tower.random, 32768));
        if (const auto cue = tower.idle.update(true, random)) {
            select(tower.sumner, cue->sumner, tower.activeSumner);
            if (!cue->podium.empty()) {
                select(tower.podium, cue->podium, tower.activePodium);
            }
        }
    }
}

void LegendsSceneAdapter::bindWorld(RenderDevice& device, const formats::LegendsMap& map,
                                    std::span<const TextureAnimationInfo> animations) {
    if (m_platform == LegendsPlatform::N64 && m_hasBossEntrance) {
        m_bossEruption = LegendsBossEruption{map.objects};
    }
    for (const auto& object : map.objects) {
        if (object.name == "CATH_BEAM") {
            m_cathedralBeam = m_hiddenWorldObjects.size();
        }
        // N64 e003955c creates flying world debris with drawable.hidden set.
        // These are dormant fragments, not a second copy of the intact floor.
        // Keep this world flag separate from the archive's authored draw flags;
        // its later reveal belongs to the native trigger, not a render heuristic.
        const bool dormantDebris =
            m_platform == LegendsPlatform::N64 && (object.flags & 0x200U) != 0;
        m_hiddenWorldObjects.push_back(
            dormantDebris || std::ranges::find(m_initiallyHiddenWorldObjects, object.name) !=
                                 m_initiallyHiddenWorldObjects.end());
    }
    m_worldTextures = textureSet(map.textures);
    std::vector<TextureSet*> lenders;
    lenders.reserve(m_archives.size());
    for (auto& archive : m_archives) {
        lenders.push_back(&archive->textures);
    }
    m_worldMotion.bind(animations, m_worldTextures, device, lenders);
    for (auto& archive : m_archives) {
        ArchiveMotion motion;
        motion.bank = &archive->textures;
        motion.animation.bind(archive->trees.textureAnimations(), archive->textures, device,
                              lenders);
        m_archiveMotion.push_back(std::move(motion));
    }
    for (u32 slot = 0; slot < m_worldTextures.size(); ++slot) {
        TextureView view;
        TextureSet* source = &m_worldTextures;
        u32 index = slot;
        const auto& entry = m_worldTextures.entry(slot);
        if (entry.external()) {
            source = nullptr;
            for (auto* lender : lenders) {
                if (const auto found = lender->find(entry.name);
                    found && !lender->entry(*found).external()) {
                    source = lender;
                    index = *found;
                    break;
                }
            }
        }
        if (source) {
            view.texture = &source->texture(device, index);
            const auto& image = source->image(index);
            for (usize i = 3; i < image.pixels.size(); i += 4) {
                view.hasTransparency |= image.pixels[i] < 255;
                view.softAlpha |= image.pixels[i] > 0 && image.pixels[i] < 255;
            }
        }
        m_worldViews.push_back(view);
        m_worldBindings.push_back({source, index});
    }
    lenders.push_back(&m_worldTextures);
    for (auto* set : lenders) {
        for (u32 i = 0; i < set->size(); ++i) {
            const auto* image = set->cachedImage(i);
            const auto* texture = set->cachedTexture(i);
            if (!image || !texture) {
                continue;
            }
            TextureView coverage;
            coverage.texture = texture;
            for (usize pixel = 3; pixel < image->pixels.size(); pixel += 4) {
                coverage.hasTransparency |= image->pixels[pixel] < 255;
                coverage.softAlpha |= image->pixels[pixel] > 0 && image->pixels[pixel] < 255;
            }
            m_coverage.push_back(coverage);
        }
    }
}

void LegendsSceneAdapter::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    m_worldMotion.advance(seconds);
    m_scenery.update(seconds);
    m_worldMovement.update(seconds);
    updateRiders();
    updateDeaths();
    updateSwitches();
    for (auto& archive : m_archiveMotion) {
        archive.animation.advance(seconds);
    }
    for (auto& scroll : m_nativeScrolls) {
        scroll.state.advance(scroll.definition, seconds);
    }
    for (usize i = 0; i < m_fixtures.size(); ++i) {
        if (!m_hiddenFixtures[i] && !m_heldFixtures[i]) {
            m_fixtures[i].update(seconds);
        }
    }
    updateTower();
    updatePortals();
    updateContainerSelectors(seconds);
    updateRiders();
    updateContainers();
    updateGates();
    updateObelisks(seconds);
    updateArena(seconds);
}

void LegendsSceneAdapter::capturePresentation() {
    m_worldMovement.capturePresentation();
    m_bossEruption.capturePresentation();
    for (auto& portal : m_towerPortals) {
        portal.capturePresentation();
    }
    for (auto& fixture : m_fixtures) {
        fixture.capturePresentation();
    }
}

Vec3 LegendsSceneAdapter::resolveMovement(const Vec3& from, const Vec3& to, f32 radius,
                                          f32 height) const {
    Vec3 result = to;
    for (const auto& door : m_towerDoors) {
        if (door.motion.state() == LegendsTowerDoorMotion::State::Closed) {
            result = resolveLegendsTowerDoor(door.definition, from, result, radius, height);
        }
    }
    for (const auto& volume : m_towerResidentVolumes) {
        result = resolveLegendsTowerVolume(volume, from, result, radius, height);
    }
    return result;
}

bool LegendsSceneAdapter::worldObjectVisible(usize object) const {
    if (const auto erupted = m_bossEruption.visible(object)) {
        return *erupted && m_scenery.visible(object);
    }
    return m_scenery.visible(object) &&
           (object >= m_hiddenWorldObjects.size() || !m_hiddenWorldObjects[object]);
}

LegendsSceneAdapter::TextureView LegendsSceneAdapter::texture(u32 slot) const {
    auto view = slot < m_worldViews.size() ? m_worldViews[slot] : TextureView{};
    const auto selectFrame = [&](const Texture* frame) {
        view.texture = frame;
        for (const auto& coverage : m_coverage) {
            if (coverage.texture == frame) {
                view.hasTransparency = coverage.hasTransparency;
                view.softAlpha = coverage.softAlpha;
                break;
            }
        }
    };
    if (slot < m_worldBindings.size()) {
        const auto& binding = m_worldBindings[slot];
        // N64 e0026674/e0026924 resolves external slots by name into a shared
        // animated lender. Retaining only its seeded GPU image freezes fire.
        for (const auto& archive : m_archiveMotion) {
            if (archive.bank != binding.bank) {
                continue;
            }
            for (usize i = 0; i < archive.animation.size(); ++i) {
                if (!archive.animation.keyed(i) && archive.animation.slot(i) == binding.slot) {
                    if (const auto* frame = archive.animation.motion(i).frame) {
                        selectFrame(frame);
                    }
                }
            }
        }
    }
    for (usize i = 0; i < m_worldMotion.size(); ++i) {
        if (m_worldMotion.slot(i) != slot || m_worldMotion.keyed(i)) {
            continue;
        }
        const auto motion = m_worldMotion.motion(i);
        if (motion.frame) {
            selectFrame(motion.frame);
        }
        const auto direction = m_worldMotion.scrollDirection(i);
        if (direction.x != 0) {
            view.offset.x = motion.offset.x;
            view.scale.x = motion.scale.x;
        }
        if (direction.y != 0) {
            view.offset.y = motion.offset.y;
            view.scale.y = motion.scale.y;
        }
    }
    for (const auto& scroll : m_nativeScrolls) {
        if (scroll.definition.texture == slot) {
            view.offset += scroll.state.offset(scroll.definition);
        }
    }
    return view;
}

void LegendsSceneAdapter::draw(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                               TreeModel::Pass pass, f32 presentationAlpha) const {
    for (usize i = 0; i < m_fixtures.size(); ++i) {
        drawFixture(i, device, clip, camera, pass, presentationAlpha);
    }
    if (TreeModel::compositesEffects(pass)) {
        drawObeliskEffects(device, clip, camera);
    }
}

std::vector<std::string> LegendsSceneAdapter::collect(PlayerActor& actor, f32 powerupTime,
                                                      bool activatePowerups,
                                                      const PickupObserver& observer,
                                                      std::span<CharacterSave* const> party) {
    std::vector<std::string> taken;
    touchGates(actor);
    touchContainers(actor);
    touchDeaths(actor.position(), actor.radius(), actor.height());
    for (usize i = 0; i < m_pickups.size(); ++i) {
        auto& pickup = m_pickups[i];
        if (!touchesLegendsPickup(pickup, actor.followPoint(), actor.reach(),
                                  actor.height() * .5f)) {
            continue;
        }
        prepareSelectorPickup(i);
        if (!touchesLegendsPickup(pickup, actor.followPoint(), actor.reach(),
                                  actor.height() * .5f)) {
            continue;
        }
        const auto offered = pickup.offer;
        const auto result = takeLegendsPickup(pickup, m_platform, actor.save(), powerupTime,
                                              activatePowerups, party);
        if (result.took()) {
            m_hiddenFixtures[m_pickupFigures[i]] = pickup.collected && !pickup.staysVisible;
            taken.push_back(pickup.name.empty() ? "Item collected" : pickup.name);
            if (observer) {
                // Partial key stacks retain their remainder in the world, but
                // feedback describes the original offer, not that leftover.
                auto receipt = pickup;
                receipt.offer = offered;
                observer(receipt, result);
            }
            if (!pickup.scrollPages.empty()) {
                // Keep overlapping scrolls for the next visit after this one closes.
                break;
            }
        }
    }
    updateContainers();
    updateObelisks();
    return taken;
}

} // namespace gdl::game
