#include "game/scenes/SceneDescription.h"

#include <exception>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "engine/assets/ModelSet.h"
#include "engine/assets/ObjectMaterial.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Mesh.h"
#include "engine/world/WorldScene.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Progression.h"
#include "game/console/FixtureRequest.h"
#include "game/critters/CombatantKind.h"
#include "game/critters/Critters.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/Generators.h"
#include "game/level/Chests.h"
#include "game/level/ImportedLevel.h"
#include "game/level/ImportedWorldObject.h"
#include "game/level/InstanceNames.h"
#include "game/level/LevelTriggers.h"
#include "game/level/LevelWorld.h"
#include "game/level/LockedGates.h"
#include "game/level/PlacedItems.h"
#include "game/level/SpawnedObjects.h"
#include "game/players/PlayerActor.h"
#include "game/scenes/FixtureConsoleCommands.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SceneSelection.h"

namespace gdl::game {

namespace {
using Lines = std::vector<std::string>;

/** Where a thing stands, as the description says it. */
std::string place(const Vec3& at) {
    return std::format("at {:.1f} {:.1f} {:.1f}", at.x, at.y, at.z);
}

/** What is said of a selection whose thing has gone. */
std::string gone(const SceneSelection& selection) {
    return std::format("{} is no longer there", selection.token());
}

/** A switch: its pad, what it drives and how it stands. */
Lines describeSwitch(const LevelWorld& world, const SceneSelection& selection) {
    Lines lines;
    const s32 id = selection.id;
    const auto switches = switchesInPlay(world);
    if (id < 0 || static_cast<usize>(id) >= switches.size()) {
        lines.push_back(
            std::format("{} names no switch of {}", selection.token(), world.ref().name));
        return lines;
    }
    const SwitchEntry& entry = switches[static_cast<usize>(id)];
    const LevelTrigger& trigger = world.triggers().trigger(static_cast<usize>(id));
    lines.push_back(
        std::format("{} {}: switch {} of {}", selection.token(), entry.name, id, world.ref().name));
    lines.push_back(
        std::format("{}; id {}; drives {}; {}{}{}{}", place(Vec3{trigger.placement[3]}), entry.id,
                    entry.target.empty() ? std::string("nothing") : entry.target,
                    entry.fired ? "fired" : "waiting", entry.shootable ? ", shootable" : "",
                    entry.chained ? ", fired through another" : "",
                    entry.enabled ? "" : ", not for this party"));
    lines.emplace_back("activate $SELECTED presses it");
    return lines;
}

/** A door a key opens, and how far it stands open. */
Lines describeGate(const PlayScene& scene, const LevelWorld& world,
                   const SceneSelection& selection) {
    Lines lines;
    const s32 id = selection.id;
    const LockedGates& gates = scene.gates();
    if (id < 0 || static_cast<usize>(id) >= gates.size()) {
        lines.push_back(std::format("{} names no door of {}", selection.token(), world.ref().name));
        return lines;
    }
    const LockedGates::Gate& gate = gates.gate(static_cast<usize>(id));
    const std::string name = instanceName(world.layout(), gate.instance);
    lines.push_back(
        std::format("{} {}: door {} of {}", selection.token(), name, id, world.ref().name));
    lines.push_back(std::format("{}; {}", place(gate.figure.position()),
                                gate.state == LockedGates::kShut      ? "shut: a key opens it"
                                : gate.state == LockedGates::kOpening ? "swinging open"
                                                                      : "open"));
    lines.emplace_back("unlock $SELECTED opens it without a key; lock $SELECTED shuts it");
    return lines;
}

/** A chest, and whether it wants a key, opens or has gone. */
Lines describeChest(const PlayScene& scene, const LevelWorld& world,
                    const SceneSelection& selection) {
    Lines lines;
    const s32 id = selection.id;
    const Chests& chests = scene.chests();
    if (id < 0 || static_cast<usize>(id) >= chests.size()) {
        lines.push_back(
            std::format("{} names no chest of {}", selection.token(), world.ref().name));
        return lines;
    }
    const Chests::Chest& chest = chests.chest(static_cast<usize>(id));
    const std::string name = instanceName(world.layout(), chest.instance);
    lines.push_back(
        std::format("{} {}: chest {} of {}", selection.token(), name, id, world.ref().name));
    lines.push_back(std::format("{}; {}{}", place(chest.figure.position()),
                                chest.state == Chests::kShut
                                    ? (chest.locked ? "shut: a key opens it" : "shut: unlocked")
                                : chest.state == Chests::kOpening ? "opening"
                                                                  : "open",
                                chest.gone ? ", gone" : ""));
    lines.emplace_back("unlock $SELECTED lets it open without a key; lock $SELECTED asks one");
    return lines;
}

/** A piece of an imported level's scenery, as the level that holds it tells it. */
Lines describeImportedObject(const ImportedLevel& imported, const LevelWorld& world,
                             const SceneSelection& selection) {
    Lines lines;
    const s32 id = selection.id;
    const auto object = imported.object(id);
    if (!object.has_value()) {
        lines.push_back(gone(selection));
        return lines;
    }
    lines.push_back(std::format("{} {}: object {} of {}", selection.token(), object->name, id,
                                world.ref().name));
    lines.push_back(place(object->position));
    lines.push_back(std::format("flags {:#x}; draw flags {:#x}; {} triangles; {}", object->flags,
                                object->drawFlags, object->triangles,
                                object->visible ? "shown" : "hidden"));
    return lines;
}

/** The mesh of the model named `name` in `meshes`, decoding it the first time: its triangles,
 * its parts and the textures they wear. */
std::string meshLine(LevelWorld& meshes, const std::string& name) {
    ModelSet& models = meshes.models();
    const TextureSet& textures = meshes.textures();
    const auto model = models.find(name);
    if (!model.has_value()) {
        return "no mesh";
    }
    const ModelSetEntry& entry = models.entry(*model);
    try {
        const Mesh& mesh = models.mesh(*model);
        std::string names;
        for (const MeshPart& part : mesh.parts) {
            const std::string texture = part.texture < textures.size()
                                            ? textures.entry(part.texture).name
                                            : std::format("texture {}", part.texture);
            if (names.find(texture) == std::string::npos) {
                names += (names.empty() ? "" : ", ") + texture;
            }
        }
        return std::format("mesh: {} triangles, {} part{}: {}", entry.triangles, mesh.parts.size(),
                           mesh.parts.size() == 1 ? "" : "s", names);
    } catch (const std::exception&) {
        return std::format("mesh: {} triangles, not decoded", entry.triangles);
    }
}

/** An object of the level's layout: where it stands, its flags and how it is drawn, and its
 * mesh when `meshes` is given. */
Lines describeObject(const LevelWorld& world, const SceneSelection& selection, LevelWorld* meshes) {
    Lines lines;
    const s32 id = selection.id;
    const auto& objects = world.layout().objects();
    if (id < 0 || static_cast<usize>(id) >= objects.size()) {
        lines.push_back(
            std::format("{} names no object of {}", selection.token(), world.ref().name));
        return lines;
    }
    const auto index = static_cast<usize>(id);
    const WorldObject& object = objects[index];
    const WorldScene& view = world.scene();
    lines.push_back(std::format("{} {}: object {} of {}", selection.token(), object.name, id,
                                world.ref().name));
    const std::string parent =
        object.parent >= 0 && static_cast<usize>(object.parent) < objects.size()
            ? std::format("{} (object {})", objects[static_cast<usize>(object.parent)].name,
                          object.parent)
            : std::string("none");
    lines.push_back(std::format("{}; radius {:.1f}; parent {}",
                                place(Vec3{view.worldTransform(index)[3]}), object.radius, parent));
    std::string flags;
    for (const auto& [bit, word] :
         {std::pair{WorldObject::kPrelit, "prelit"}, std::pair{WorldObject::kFloor, "floor"},
          std::pair{WorldObject::kParticles, "particles"},
          std::pair{WorldObject::kAnimated, "animated"},
          std::pair{WorldObject::kReverse, "reverse"}, std::pair{WorldObject::kOnce, "once"}}) {
        if ((object.flags & bit) != 0) {
            flags += std::format(" {}", word);
        }
    }
    const ObjectMaterial material = ObjectMaterial::fromFlags(object.objectFlags);
    std::string look;
    if (material.chrome) {
        look += " chrome";
    }
    if (material.additive) {
        look += " additive";
    }
    if (material.sorted) {
        look += " sorted";
    }
    if (!material.depthWrite) {
        look += " no-depth-write";
    }
    if (!material.depthTest) {
        look += " no-depth-test";
    }
    if (material.facing != 0) {
        look += std::format(" facing-{}", material.facing);
    }
    lines.push_back(std::format("flags {:#x}{}; object flags {:#x}{}", object.flags, flags,
                                object.objectFlags, look));
    lines.push_back(std::format("{}; {}",
                                view.moving(index) ? "animated: placed every frame" : "static",
                                view.objectVisible(index) ? "shown" : "hidden"));
    // A shared scene's models are never decoded by inspection.
    if (meshes != nullptr) {
        lines.push_back(meshLine(*meshes, object.name));
    }
    return lines;
}

/** One of the swarm: its kind, tier and health, and its body. */
Lines describeEnemy(const Enemies& swarm, const SceneSelection& selection) {
    Lines lines;
    const s32 id = selection.id;
    if (id < 0 || id >= Enemies::kMost || !swarm.alive(id)) {
        lines.push_back(gone(selection));
        return lines;
    }
    const EnemyKind& kind = enemyKind(swarm.kindOf(id));
    lines.push_back(std::format("{} {}: tier {}, health {:.0f}{}", selection.token(), kind.name,
                                swarm.tierOf(id), swarm.healthOf(id),
                                swarm.dying(id) ? ", dying" : ""));
    lines.push_back(std::format("{}; radius {:.1f}, height {:.1f}", place(swarm.positionOf(id)),
                                swarm.radiusOf(id), swarm.heightOf(id)));
    return lines;
}

/** A great one: its kind, form and health, and its body. */
Lines describeCritter(const Critters& critters, const SceneSelection& selection) {
    Lines lines;
    const s32 id = selection.id;
    if (id < 0 || static_cast<usize>(id) >= critters.count() || !critters.alive(id)) {
        lines.push_back(gone(selection));
        return lines;
    }
    const std::string form = critters.formOf(id);
    lines.push_back(std::format(
        "{} {}{}: health {:.0f}", selection.token(), combatantKindName(critters.kindOf(id)),
        form.empty() ? "" : std::format(" {}", form), critters.healthOf(id)));
    lines.push_back(
        std::format("{}; radius {:.1f}", place(critters.positionOf(id)), critters.radiusOf(id)));
    return lines;
}

/** A generator: what it breeds, its tier, state and health. */
Lines describeGenerator(const Generators& generators, const SceneSelection& selection) {
    Lines lines;
    const s32 id = selection.id;
    if (id < 0 || static_cast<usize>(id) >= generators.count()) {
        lines.push_back(gone(selection));
        return lines;
    }
    const s32 kind = generators.kindOf(id);
    lines.push_back(std::format("{} {}: tier {}, state {} of {}, health {:.0f}", selection.token(),
                                kind >= 0 ? enemyKind(kind).name : "brood", generators.tierOf(id),
                                generators.stateOf(id), Generators::kStates,
                                generators.healthOf(id)));
    lines.push_back(place(generators.positionOf(id)));
    return lines;
}

/** A placed pickup: its record, its worth and whether it is taken or unseen. */
Lines describePickup(const PlacedItems& items, const SceneSelection& selection) {
    Lines lines;
    const s32 id = selection.id;
    if (id < 0 || static_cast<usize>(id) >= items.size()) {
        lines.push_back(gone(selection));
        return lines;
    }
    const PlacedItems::Item& item = items.item(static_cast<usize>(id));
    lines.push_back(std::format("{} {}: {}, worth {}{}{}", selection.token(), item.name,
                                pickupKindName(item.subtype), item.value,
                                item.taken ? ", taken" : "", item.visible ? "" : ", unseen"));
    lines.push_back(std::format("{}; radius {:.1f}, height {:.1f}; instance {} of record {}",
                                place(item.position), item.radius, item.height, item.instance,
                                item.info));
    return lines;
}

/** A player: the character, its level and health, and its body. */
Lines describePlayer(const PlayScene& scene, const SceneSelection& selection) {
    Lines lines;
    const PlayerActor* actor = scene.actor(selection.id);
    if (actor == nullptr) {
        lines.push_back(gone(selection));
        return lines;
    }
    const CharacterSave& save = actor->save();
    lines.push_back(std::format("{} {}: {} of level {}, health {}", selection.token(), save.name,
                                classCode(save.character % kStartingClassCount),
                                experienceLevel(save.experience()), save.health()));
    lines.push_back(std::format("{}; facing {:.0f} degrees; radius {:.1f}, height {:.1f}",
                                place(actor->position()), glm::degrees(actor->yaw()),
                                actor->radius(), actor->height()));
    return lines;
}

/** A copy of an object the console stood. */
Lines describeSpawned(const SpawnedObjects& spawned, const SceneSelection& selection) {
    Lines lines;
    const s32 id = selection.id;
    if (id < 0 || static_cast<usize>(id) >= spawned.count()) {
        lines.push_back(gone(selection));
        return lines;
    }
    const SpawnedObjects::View view = spawned.view(static_cast<usize>(id));
    lines.push_back(std::format("{} {}: stood by the console", selection.token(), view.name));
    lines.push_back(std::format("{}; radius {:.1f}", place(view.position), view.radius));
    return lines;
}
} // namespace

std::vector<std::string> describeSelection(const PlayScene& scene, const LevelWorld& world,
                                           const SceneSelection& selection, LevelWorld* meshes) {
    using Kind = SceneSelection::Kind;
    switch (selection.kind) {
    case Kind::Switch: return describeSwitch(world, selection);
    case Kind::Gate: return describeGate(scene, world, selection);
    case Kind::Chest: return describeChest(scene, world, selection);
    case Kind::Object:
        if (const ImportedLevel* imported = world.imported()) {
            return describeImportedObject(*imported, world, selection);
        }
        return describeObject(world, selection, meshes);
    case Kind::Enemy: return describeEnemy(scene.enemies(), selection);
    case Kind::Critter: return describeCritter(scene.critters(), selection);
    case Kind::Generator: return describeGenerator(scene.generators(), selection);
    case Kind::Pickup: return describePickup(world.placedItems(), selection);
    case Kind::Player: return describePlayer(scene, selection);
    case Kind::Spawned: return describeSpawned(scene.spawnedObjects(), selection);
    }
    return {};
}

} // namespace gdl::game
