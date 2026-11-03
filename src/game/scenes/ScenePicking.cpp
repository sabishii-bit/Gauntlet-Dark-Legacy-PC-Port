#include "game/scenes/ScenePicking.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <utility>

#include "engine/assets/ModelSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/render/Mesh.h"
#include "engine/world/SelectionHighlight.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldScene.h"

#include "game/critters/CombatantKind.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/LevelOpponents.h"
#include "game/level/ImportedLevel.h"
#include "game/level/InstanceNames.h"
#include "game/level/LevelFixtures.h"
#include "game/level/LevelWorld.h"
#include "game/level/PlacedItems.h"
#include "game/level/SpawnedObjects.h"
#include "game/party/PlayerRuntime.h"

namespace gdl::game {

namespace {

constexpr f32 kTie = 0.05f;            ///< hits this close along the ray are the same distance
constexpr f32 kGeneratorReach = 3.0f;  ///< a generator's body, round its placement
constexpr f32 kGeneratorHeight = 2.0f; ///< where that sphere's centre stands above it
constexpr f32 kPickupReach = 0.75f;    ///< the least a pickup is reached by

} // namespace

std::optional<ScenePicking::Ray> ScenePicking::cursorRay(Vec2 cursor, const Mat4& clip,
                                                         const Vec3& eye) {
    if (cursor.x < 0.0f || cursor.x > 1.0f || cursor.y < 0.0f || cursor.y > 1.0f) {
        return std::nullopt;
    }
    const Mat4 unclip = glm::inverse(clip);
    const Vec2 ndc = cursor * 2.0f - Vec2{1.0f};
    const auto at = [&](f32 depth) -> std::optional<Vec3> {
        const Vec4 homogeneous = unclip * Vec4{ndc, depth, 1.0f};
        if (std::abs(homogeneous.w) < 1.0e-9f) {
            return std::nullopt;
        }
        return Vec3{homogeneous} / homogeneous.w;
    };
    const auto one = at(0.0f);
    const auto other = at(1.0f);
    if (!one.has_value() || !other.has_value()) {
        return std::nullopt;
    }
    // Whichever end lies further from the eye is the far one: the ray leaves the eye for it.
    const Vec3& far = glm::distance(*one, eye) > glm::distance(*other, eye) ? *one : *other;
    const Vec3 direction = far - eye;
    const f32 length = glm::length(direction);
    if (!(length > 1.0e-6f)) {
        return std::nullopt;
    }
    return Ray{eye, direction / length};
}

std::optional<f32> ScenePicking::sphereHit(const Ray& ray, const Vec3& centre, f32 radius) {
    const Vec3 toOrigin = ray.origin - centre;
    const f32 b = glm::dot(toOrigin, ray.direction);
    const f32 c = glm::dot(toOrigin, toOrigin) - radius * radius;
    const f32 discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return std::nullopt;
    }
    const f32 root = std::sqrt(discriminant);
    const f32 entry = -b - root;
    if (entry >= 0.0f) {
        return entry;
    }
    return -b + root >= 0.0f ? std::optional{0.0f} : std::nullopt;
}

std::optional<f32> ScenePicking::triangleHit(const Ray& ray, const Vec3& a, const Vec3& b,
                                             const Vec3& c) {
    constexpr f32 kParallel = 1.0e-7f;
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 p = glm::cross(ray.direction, ac);
    const f32 determinant = glm::dot(ab, p);
    if (std::abs(determinant) < kParallel) {
        return std::nullopt;
    }
    const f32 inverse = 1.0f / determinant;
    const Vec3 s = ray.origin - a;
    const f32 u = glm::dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return std::nullopt;
    }
    const Vec3 q = glm::cross(s, ab);
    const f32 v = glm::dot(ray.direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return std::nullopt;
    }
    const f32 t = glm::dot(ac, q) * inverse;
    return t >= 0.0f ? std::optional{t} : std::nullopt;
}

std::optional<f32> ScenePicking::meshHit(const Ray& ray, const Mesh& mesh, const Mat4& placement) {
    std::optional<f32> nearest;
    for (const MeshPart& part : mesh.parts) {
        for (usize i = 0; i + 2 < part.indices.size(); i += 3) {
            const Vec3 a{placement * Vec4{mesh.vertices[part.indices[i]].position, 1.0f}};
            const Vec3 b{placement * Vec4{mesh.vertices[part.indices[i + 1]].position, 1.0f}};
            const Vec3 c{placement * Vec4{mesh.vertices[part.indices[i + 2]].position, 1.0f}};
            if (const auto hit = triangleHit(ray, a, b, c);
                hit.has_value() && (!nearest.has_value() || *hit < *nearest)) {
                nearest = hit;
            }
        }
    }
    return nearest;
}

std::optional<SceneSelection> ScenePicking::pick(const Scene& scene, Vec2 cursor, const Mat4& clip,
                                                 const WorldCamera& view, f32 worldAlpha) {
    if (scene.world == nullptr) {
        return std::nullopt;
    }
    const auto ray = cursorRay(cursor, clip, view.position);
    if (!ray.has_value()) {
        return std::nullopt;
    }
    struct Best {
        f32 distance = 0.0f;
        f32 size = 0.0f;
        SceneSelection what;
    };
    std::optional<Best> best;
    // The nearest wins; of two as near, the smaller, which the other may stand round.
    const auto consider = [&](std::optional<f32> distance, f32 size, SceneSelection what) {
        if (!distance.has_value()) {
            return;
        }
        if (!best.has_value() || *distance < best->distance - kTie ||
            (std::abs(*distance - best->distance) <= kTie && size < best->size)) {
            best = Best{*distance, size, std::move(what)};
        }
    };
    const auto body = [&](const Vec3& feet, f32 radius, f32 height) {
        const f32 reach = std::max(radius, height * 0.5f);
        return std::pair{sphereHit(*ray, feet + Vec3{0.0f, height * 0.5f, 0.0f}, reach), reach};
    };
    // The level's objects, by their faces: a bounding sphere alone would hand a click on a
    // small thing to the floor it stands on.
    const WorldLayout& layout = scene.world->layout();
    if (const auto* imported = scene.world->imported()) {
        if (const auto hit = imported->pickObject(ray->origin, ray->direction, view, worldAlpha)) {
            consider(
                hit->distance, 0,
                SceneSelection{SceneSelection::Kind::Object, hit->object.id, hit->object.name});
        }
    }
    const WorldScene& worldScene = scene.world->scene();
    ModelSet& models = scene.world->models();
    const auto& objects = layout.objects();
    for (usize i = 0; i < objects.size(); ++i) {
        const WorldObject& object = objects[i];
        if (object.particles() || !worldScene.objectVisible(i)) {
            continue;
        }
        const auto model = models.find(object.name);
        if (!model.has_value()) {
            continue;
        }
        const Mesh* mesh = nullptr;
        try {
            mesh = &models.mesh(*model);
        } catch (const std::exception&) {
            continue;
        }
        const Mat4& placement = worldScene.worldTransform(i);
        if (object.radius > 0.0f &&
            !sphereHit(*ray, Vec3{placement[3]}, object.radius).has_value()) {
            continue;
        }
        consider(meshHit(*ray, *mesh, placement), object.radius,
                 SceneSelection{SceneSelection::Kind::Object, static_cast<s32>(i), object.name});
    }
    const Enemies& swarm = scene.opponents.enemies();
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (!swarm.alive(id)) {
            continue;
        }
        const auto [hit, size] = body(swarm.positionOf(id), swarm.radiusOf(id), swarm.heightOf(id));
        consider(hit, size,
                 SceneSelection{SceneSelection::Kind::Enemy, id,
                                std::string(enemyKind(swarm.kindOf(id)).name)});
    }
    const Critters& critters = scene.opponents.critters();
    for (s32 id = 0; id < static_cast<s32>(critters.count()); ++id) {
        if (!critters.alive(id)) {
            continue;
        }
        const f32 radius = critters.radiusOf(id);
        const auto [hit, size] = body(critters.positionOf(id), radius, radius * 2.0f);
        const std::string form = critters.formOf(id);
        consider(hit, size,
                 SceneSelection{SceneSelection::Kind::Critter, id,
                                form.empty() ? std::string(combatantKindName(critters.kindOf(id)))
                                             : form});
    }
    for (const auto& shown : scene.opponents.generators().presentation()) {
        const Vec3 centre = Vec3{shown.placement[3]} + Vec3{0.0f, kGeneratorHeight, 0.0f};
        consider(sphereHit(*ray, centre, kGeneratorReach), kGeneratorReach,
                 SceneSelection{SceneSelection::Kind::Generator, static_cast<s32>(shown.index),
                                shown.kind >= 0 ? std::string(enemyKind(shown.kind).name)
                                                : std::string("brood")});
    }
    const PlacedItems& items = scene.world->placedItems();
    for (usize i = 0; i < items.size(); ++i) {
        const PlacedItems::Item& item = items.item(i);
        if (!item.visible || item.taken || item.carried) {
            continue;
        }
        const auto [hit, size] = body(item.position, std::max(item.radius, kPickupReach),
                                      std::max(item.height, kPickupReach));
        consider(hit, size,
                 SceneSelection{SceneSelection::Kind::Pickup, static_cast<s32>(i), item.name});
    }
    // The switches' pads, the gates and the chests: the console names what they drive.
    const LevelTriggers& triggers = scene.world->triggers();
    for (usize i = 0; i < triggers.size(); ++i) {
        const LevelTrigger& trigger = triggers.trigger(i);
        if (trigger.chained || !trigger.enabled) {
            continue;
        }
        const auto [hit, size] =
            body(Vec3{trigger.placement[3]}, std::max(trigger.radius, kPickupReach),
                 std::max(trigger.height, kPickupReach));
        consider(hit, size,
                 SceneSelection{SceneSelection::Kind::Switch, static_cast<s32>(i),
                                instanceName(layout, trigger.instance)});
    }
    const LockedGates& gates = scene.fixtures.gates();
    for (usize i = 0; i < gates.size(); ++i) {
        const LockedGates::Gate& gate = gates.gate(i);
        if (!gate.shown) {
            continue;
        }
        const f32 reach = std::max({gate.box.halfAcross, gate.box.halfAlong, kPickupReach});
        const auto [hit, size] = body(gate.figure.position(), reach, reach * 2.0f);
        consider(hit, size,
                 SceneSelection{SceneSelection::Kind::Gate, static_cast<s32>(i),
                                instanceName(layout, gate.instance)});
    }
    const Chests& chests = scene.fixtures.chests();
    for (usize i = 0; i < chests.size(); ++i) {
        const Chests::Chest& chest = chests.chest(i);
        if (!chest.shown || chest.gone) {
            continue;
        }
        const f32 reach = std::max({chest.box.halfAcross, chest.box.halfAlong, kPickupReach});
        const auto [hit, size] = body(chest.figure.position(), reach, reach * 2.0f);
        consider(hit, size,
                 SceneSelection{SceneSelection::Kind::Chest, static_cast<s32>(i),
                                instanceName(layout, chest.instance)});
    }
    for (const PlayerRuntime& runtime : scene.players) {
        if (runtime.departed || runtime.life != PlayerLife::Standing) {
            continue;
        }
        const PlayerActor& actor = runtime.actor;
        const auto [hit, size] = body(actor.position(), actor.radius(), actor.height());
        consider(hit, size,
                 SceneSelection{SceneSelection::Kind::Player, actor.player(), actor.save().name});
    }
    for (usize i = 0; i < scene.spawned.count(); ++i) {
        const SpawnedObjects::View spawned = scene.spawned.view(i);
        consider(sphereHit(*ray, spawned.position, spawned.radius), spawned.radius,
                 SceneSelection{SceneSelection::Kind::Spawned, static_cast<s32>(i), spawned.name});
    }
    if (!best.has_value()) {
        return std::nullopt;
    }
    return best->what;
}

void ScenePicking::markSelection(const Scene& scene, const SelectionHighlight* highlight) const {
    using Kind = SceneSelection::Kind;
    const auto idOf = [&](Kind kind) {
        return highlight != nullptr && m_selection.has_value() && m_selection->kind == kind
                   ? m_selection->id
                   : -1;
    };
    scene.opponents.enemies().setHighlight(idOf(Kind::Enemy), highlight);
    scene.opponents.critters().setHighlight(idOf(Kind::Critter), highlight);
    scene.opponents.generators().setHighlight(idOf(Kind::Generator), highlight);
    if (scene.world != nullptr) {
        scene.world->placedItems().setHighlight(idOf(Kind::Pickup), highlight);
    }
    scene.spawned.setHighlight(idOf(Kind::Spawned), highlight);
    const s32 player = idOf(Kind::Player);
    for (const PlayerRuntime& runtime : scene.players) {
        if (runtime.figure != nullptr) {
            runtime.figure->setHighlight(runtime.actor.player() == player ? highlight : nullptr);
        }
    }
}

} // namespace gdl::game
