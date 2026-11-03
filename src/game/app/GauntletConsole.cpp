#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/ModelSet.h"
#include "engine/assets/ObjectMaterial.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Log.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

#include "game/app/ConsoleActionRoute.h"
#include "game/app/ConsoleRoster.h"
#include "game/app/Gauntlet.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Progression.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/FixtureRequest.h"
#include "game/console/NoclipMotion.h"
#include "game/critters/CombatantKind.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/Generators.h"
#include "game/level/InstanceNames.h"
#include "game/level/PlacedItems.h"
#include "game/players/PlayerActor.h"
#include "game/scenes/FixtureConsoleCommands.h"
#include "game/scenes/SceneSelection.h"
#include "game/widgets/MenuInput.h"

namespace gdl::game {

namespace {
/** The commands a Legends map's preview lets the console run, besides the offline-only ones. */
constexpr std::array<std::string_view, 2> kPreviewWords{"help", "goto"};
} // namespace

void Gauntlet::setUpConsole() {
    if (!m_console.load(renderDevice(), m_options.unpackedDirectory)) {
        log::warn("Developer console: the 8x8 font is missing; its screen will be blank");
    }
    m_console.print(std::format("CGaunt++ {}", m_buildLabel.text()));
    m_console.print("Type help for the commands.");
    const auto playScene = [this]() -> PlayScene* {
        return m_play->scene.isOpen() ? &m_play->scene : nullptr;
    };
    const ConsoleRosterAccess access{
        .online = {.permitGuests =
                       [this](bool allow) {
                           return m_online && m_online->consoleHost() &&
                                  m_online->setConsolePermissions(allow);
                       }},
        .view = {.levelDebug = m_levelDebug,
                 .hudHidden = m_hudHidden,
                 .freeCamera = m_freeCamera,
                 .view = [this]() -> std::optional<WorldCamera> {
                     const PlayScene* scene = consoleScene();
                     return scene != nullptr ? std::optional{scene->viewCamera()} : std::nullopt;
                 },
                 .levelDebugLines = [this] { return levelDebugLines(); }},
        .listings = {.viewed = [this] { return consoleWorld(); },
                     .levels = m_levels,
                     .unpackedRoot = m_options.unpackedDirectory,
                     .save = [this]() -> const CharacterSave* {
                         const PlayScene* scene = consoleScene();
                         const PlayerActor* actor =
                             scene != nullptr ? scene->actor(consolePlayer()) : nullptr;
                         return actor != nullptr ? &actor->save() : nullptr;
                     }},
        .fixtures = {.scene = playScene, .world = [this] { return consoleWorld(); }},
        .shared = {.sink = m_consoleRoute,
                   .levels = m_levels,
                   .legendsMapNames = [this] { return legendsMapNames(); }},
        .inspect = {
            .selected = [this]() -> std::optional<SceneSelection> {
                const PlayScene* scene = consoleScene();
                return scene != nullptr ? scene->selection() : std::nullopt;
            },
            .describe =
                [this](const SceneSelection& selection) { return consoleDescribe(selection); }},
        .partyCount = {.count = m_partyCountOverride, .scene = playScene},
        .profile = {.saves = m_saves, .config = m_config, .scene = playScene},
    };
    registerConsoleRoster(m_console.host(), m_console.commands(), m_consoleListings, access,
                          [this](ConsoleHost& host, ConsoleListings& listings) {
                              registerLegendsConsole(host, listings);
                          });
}

ConsoleSession Gauntlet::consoleSession() const {
    ConsoleSituation situation{.netplay = m_netplay != nullptr};
    if (m_online) {
        situation.online = true;
        situation.host = m_online->consoleHost();
        situation.allowed = m_online->consoleAllowed();
    }
    if (m_legendsPreview.isOpen()) {
        situation.preview = kPreviewWords;
        situation.parked = true; // the preview holds the scene it was opened over
    }
    return ConsolePolicy::session(situation);
}

const PlayScene* Gauntlet::consoleScene() const {
    return m_online ? m_online->consoleScene()
                    : (m_play->scene.isOpen() ? &m_play->scene : nullptr);
}

const LevelWorld* Gauntlet::consoleWorld() const {
    return m_online ? m_online->consoleWorld()
                    : (m_play->scene.isOpen() ? &m_play->world : nullptr);
}

ConsoleActionRoute Gauntlet::consoleActionRoute() {
    return {[this](s32 seat, std::string line) -> std::optional<bool> {
                if (!m_online) {
                    return std::nullopt;
                }
                return seat >= 0 &&
                       m_online->consoleCommand(static_cast<u8>(seat), std::move(line));
            },
            [this] { return consolePlayer(); }};
}

bool Gauntlet::updateOnlineConsole(f64 deltaSeconds, bool overlay) {
    const bool wasOpen = m_console.open();
    const bool allowed = m_online && m_online->consoleAllowed() && !overlay;
    if (!allowed) {
        m_console.close();
        m_freeCamera.stop();
        window().holdCursor(false);
    } else {
        if (const auto* scene = consoleScene()) {
            if (const auto* actor = scene->actor(consolePlayer())) {
                m_console.setPrompt(actor->save().name + ">");
            }
        }
        m_console.setSession(consoleSession());
        m_console.update(deltaSeconds, input(), m_config.menu.console);
        if (m_console.open() && !wasOpen) {
            window().stopRumble();
            resetPlayInput();
        }
        // Over the screen the pointer, once it moves, sees through the console and a click
        // there picks what is beneath; elsewhere a click picks as before.
        const Vec2 cursor{input().pointer().x, input().pointer().y};
        const bool over = input().pointer().inside && consoleCovers(cursor);
        m_console.hover(cursor, over);
        if (m_console.open() && input().wasKeyPressed(Key::MouseLeft) &&
            (!over || m_console.seeThrough())) {
            consoleSelect(cursor);
        }
        if (m_freeCamera.active() && !m_console.open()) {
            m_freeCamera.update(input(), static_cast<f32>(deltaSeconds));
        }
        window().holdCursor(m_freeCamera.active() && !m_console.open() &&
                            input().isKeyDown(Key::MouseRight));
    }
    if (!allowed || (wasOpen && !m_console.open())) {
        consoleSelect(std::nullopt);
    }
    if (m_online) {
        m_online->consoleCapture(m_console.open() || wasOpen || m_freeCamera.active());
        m_online->consoleView(m_hudHidden, m_freeCamera.active()
                                               ? std::optional{m_freeCamera.camera()}
                                               : std::nullopt);
        for (const auto& reply : m_online->takeConsoleReplies()) {
            m_console.print(reply);
        }
    }
    return m_console.open() || wasOpen;
}

bool Gauntlet::consoleCovers(Vec2 cursor) {
    // The 2D layer is letterboxed into the window: as wide or as tall as the window
    // allows at the frame's shape, centred. The console spans that frame's width.
    const Extent2D window = renderDevice().framebufferExtent();
    if (window.isZero()) {
        return false;
    }
    const auto frameWidth = static_cast<f32>(m_config.display.frameWidth);
    const auto frameHeight = static_cast<f32>(m_config.display.frameHeight);
    const auto windowWidth = static_cast<f32>(window.width);
    const auto windowHeight = static_cast<f32>(window.height);
    const f32 scale = std::min(windowWidth / frameWidth, windowHeight / frameHeight);
    const f32 width = frameWidth * scale / windowWidth;
    const f32 height = frameHeight * scale / windowHeight;
    const f32 left = (1.0f - width) * 0.5f;
    const f32 top = (1.0f - height) * 0.5f;
    const f32 shown = height * m_console.slide() * static_cast<f32>(DevConsole::kHeight) /
                      static_cast<f32>(m_config.display.virtualHeight);
    return cursor.x >= left && cursor.x <= left + width && cursor.y >= top &&
           cursor.y <= top + shown;
}

void Gauntlet::consoleSelect(std::optional<Vec2> cursor) {
    std::optional<SceneSelection> picked;
    if (cursor.has_value() && consoleScene()) {
        picked = m_online ? m_online->consolePick(*cursor) : m_play->scene.pick(*cursor);
    }
    if (m_online) {
        m_online->consoleSelect(picked);
    } else if (m_play->scene.isOpen()) {
        m_play->scene.select(picked);
    }
    m_console.setVariable("SELECTED",
                          picked.has_value() ? std::optional{picked->token()} : std::nullopt);
    if (cursor.has_value()) {
        m_console.print(picked.has_value() ? std::format("Selected {}", picked->label())
                                           : "Nothing there to select");
    }
}

std::vector<std::string> Gauntlet::consoleDescribe(const SceneSelection& selection) {
    using Kind = SceneSelection::Kind;
    std::vector<std::string> lines;
    const PlayScene* active = consoleScene();
    if (active == nullptr) {
        lines.emplace_back("Nothing is in play.");
        return lines;
    }
    const PlayScene& scene = *active;
    const auto place = [](const Vec3& at) {
        return std::format("at {:.1f} {:.1f} {:.1f}", at.x, at.y, at.z);
    };
    const auto gone = [&] {
        lines.push_back(std::format("{} is no longer there", selection.token()));
    };
    const s32 id = selection.id;
    switch (selection.kind) {
    case Kind::Switch: {
        const auto switches = switchesInPlay(*consoleWorld());
        if (id < 0 || static_cast<usize>(id) >= switches.size()) {
            lines.push_back(std::format("{} names no switch of {}", selection.token(),
                                        consoleWorld()->ref().name));
            break;
        }
        const SwitchEntry& entry = switches[static_cast<usize>(id)];
        const LevelTrigger& trigger = consoleWorld()->triggers().trigger(static_cast<usize>(id));
        lines.push_back(std::format("{} {}: switch {} of {}", selection.token(), entry.name, id,
                                    consoleWorld()->ref().name));
        lines.push_back(
            std::format("{}; id {}; drives {}; {}{}{}{}", place(Vec3{trigger.placement[3]}),
                        entry.id, entry.target.empty() ? std::string("nothing") : entry.target,
                        entry.fired ? "fired" : "waiting", entry.shootable ? ", shootable" : "",
                        entry.chained ? ", fired through another" : "",
                        entry.enabled ? "" : ", not for this party"));
        lines.emplace_back("activate $SELECTED presses it");
        break;
    }
    case Kind::Gate: {
        const LockedGates& gates = scene.gates();
        if (id < 0 || static_cast<usize>(id) >= gates.size()) {
            lines.push_back(std::format("{} names no door of {}", selection.token(),
                                        consoleWorld()->ref().name));
            break;
        }
        const LockedGates::Gate& gate = gates.gate(static_cast<usize>(id));
        const std::string name = instanceName(consoleWorld()->layout(), gate.instance);
        lines.push_back(std::format("{} {}: door {} of {}", selection.token(), name, id,
                                    consoleWorld()->ref().name));
        lines.push_back(std::format("{}; {}", place(gate.figure.position()),
                                    gate.state == LockedGates::kShut      ? "shut: a key opens it"
                                    : gate.state == LockedGates::kOpening ? "swinging open"
                                                                          : "open"));
        lines.emplace_back("unlock $SELECTED opens it without a key; lock $SELECTED shuts it");
        break;
    }
    case Kind::Chest: {
        const Chests& chests = scene.chests();
        if (id < 0 || static_cast<usize>(id) >= chests.size()) {
            lines.push_back(std::format("{} names no chest of {}", selection.token(),
                                        consoleWorld()->ref().name));
            break;
        }
        const Chests::Chest& chest = chests.chest(static_cast<usize>(id));
        const std::string name = instanceName(consoleWorld()->layout(), chest.instance);
        lines.push_back(std::format("{} {}: chest {} of {}", selection.token(), name, id,
                                    consoleWorld()->ref().name));
        lines.push_back(std::format("{}; {}{}", place(chest.figure.position()),
                                    chest.state == Chests::kShut
                                        ? (chest.locked ? "shut: a key opens it" : "shut: unlocked")
                                    : chest.state == Chests::kOpening ? "opening"
                                                                      : "open",
                                    chest.gone ? ", gone" : ""));
        lines.emplace_back("unlock $SELECTED lets it open without a key; lock $SELECTED asks one");
        break;
    }
    case Kind::Object: {
        const LevelWorld& world = *consoleWorld();
        if (const auto* imported = world.imported()) {
            if (const auto object = imported->object(id)) {
                lines.push_back(std::format("{} {}: object {} of {}", selection.token(),
                                            object->name, id, world.ref().name));
                lines.push_back(place(object->position));
                lines.push_back(std::format("flags {:#x}; draw flags {:#x}; {} triangles; {}",
                                            object->flags, object->drawFlags, object->triangles,
                                            object->visible ? "shown" : "hidden"));
            } else {
                gone();
            }
            break;
        }
        const auto& objects = world.layout().objects();
        if (id < 0 || static_cast<usize>(id) >= objects.size()) {
            lines.push_back(
                std::format("{} names no object of {}", selection.token(), world.ref().name));
            break;
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
                                    place(Vec3{view.worldTransform(index)[3]}), object.radius,
                                    parent));
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
        if (m_online) {
            // Inspection must not lazily bind additional assets in a running shared scene.
            break;
        }
        ModelSet& models = m_play->world.models();
        const TextureSet& textures = m_play->world.textures();
        const auto model = models.find(object.name);
        if (!model.has_value()) {
            lines.emplace_back("no mesh");
            break;
        }
        const ModelSetEntry& entry = models.entry(*model);
        try {
            const Mesh& mesh = models.mesh(*model);
            std::string names;
            for (const MeshPart& part : mesh.parts) {
                const std::string name = part.texture < textures.size()
                                             ? textures.entry(part.texture).name
                                             : std::format("texture {}", part.texture);
                if (names.find(name) == std::string::npos) {
                    names += (names.empty() ? "" : ", ") + name;
                }
            }
            lines.push_back(std::format("mesh: {} triangles, {} part{}: {}", entry.triangles,
                                        mesh.parts.size(), mesh.parts.size() == 1 ? "" : "s",
                                        names));
        } catch (const std::exception&) {
            lines.push_back(std::format("mesh: {} triangles, not decoded", entry.triangles));
        }
        break;
    }
    case Kind::Enemy: {
        const Enemies& swarm = scene.enemies();
        if (id < 0 || id >= Enemies::kMost || !swarm.alive(id)) {
            gone();
            break;
        }
        const EnemyKind& kind = enemyKind(swarm.kindOf(id));
        lines.push_back(std::format("{} {}: tier {}, health {:.0f}{}", selection.token(), kind.name,
                                    swarm.tierOf(id), swarm.healthOf(id),
                                    swarm.dying(id) ? ", dying" : ""));
        lines.push_back(std::format("{}; radius {:.1f}, height {:.1f}", place(swarm.positionOf(id)),
                                    swarm.radiusOf(id), swarm.heightOf(id)));
        break;
    }
    case Kind::Critter: {
        const Critters& critters = scene.critters();
        if (id < 0 || static_cast<usize>(id) >= critters.count() || !critters.alive(id)) {
            gone();
            break;
        }
        const std::string form = critters.formOf(id);
        lines.push_back(std::format(
            "{} {}{}: health {:.0f}", selection.token(), combatantKindName(critters.kindOf(id)),
            form.empty() ? "" : std::format(" {}", form), critters.healthOf(id)));
        lines.push_back(std::format("{}; radius {:.1f}", place(critters.positionOf(id)),
                                    critters.radiusOf(id)));
        break;
    }
    case Kind::Generator: {
        const Generators& generators = scene.generators();
        if (id < 0 || static_cast<usize>(id) >= generators.count()) {
            gone();
            break;
        }
        const s32 kind = generators.kindOf(id);
        lines.push_back(std::format("{} {}: tier {}, state {} of {}, health {:.0f}",
                                    selection.token(), kind >= 0 ? enemyKind(kind).name : "brood",
                                    generators.tierOf(id), generators.stateOf(id),
                                    Generators::kStates, generators.healthOf(id)));
        lines.push_back(place(generators.positionOf(id)));
        break;
    }
    case Kind::Pickup: {
        const PlacedItems& items = consoleWorld()->placedItems();
        if (id < 0 || static_cast<usize>(id) >= items.size()) {
            gone();
            break;
        }
        const PlacedItems::Item& item = items.item(static_cast<usize>(id));
        lines.push_back(std::format("{} {}: {}, worth {}{}{}", selection.token(), item.name,
                                    pickupKindName(item.subtype), item.value,
                                    item.taken ? ", taken" : "", item.visible ? "" : ", unseen"));
        lines.push_back(std::format("{}; radius {:.1f}, height {:.1f}; instance {} of record {}",
                                    place(item.position), item.radius, item.height, item.instance,
                                    item.info));
        break;
    }
    case Kind::Player: {
        const PlayerActor* actor = scene.actor(id);
        if (actor == nullptr) {
            gone();
            break;
        }
        const CharacterSave& save = actor->save();
        lines.push_back(std::format("{} {}: {} of level {}, health {}", selection.token(),
                                    save.name, classCode(save.character % kStartingClassCount),
                                    experienceLevel(save.experience()), save.health()));
        lines.push_back(std::format("{}; facing {:.0f} degrees; radius {:.1f}, height {:.1f}",
                                    place(actor->position()), glm::degrees(actor->yaw()),
                                    actor->radius(), actor->height()));
        break;
    }
    case Kind::Spawned: {
        const SpawnedObjects& spawned = scene.spawnedObjects();
        if (id < 0 || static_cast<usize>(id) >= spawned.count()) {
            gone();
            break;
        }
        const SpawnedObjects::View view = spawned.view(static_cast<usize>(id));
        lines.push_back(std::format("{} {}: stood by the console", selection.token(), view.name));
        lines.push_back(std::format("{}; radius {:.1f}", place(view.position), view.radius));
        break;
    }
    }
    return lines;
}

void Gauntlet::applyConsoleState(f64 deltaSeconds, bool consoleOpen) {
    m_play->scene.setHudHidden(m_hudHidden);
    if (m_freeCamera.active() && !consoleOpen) {
        m_freeCamera.update(input(), static_cast<f32>(deltaSeconds));
    }
    // While the free camera is turned by the right button, the pointer is hidden where
    // it is and shows there again when the button is let go.
    window().holdCursor(m_freeCamera.active() && !consoleOpen &&
                        input().isKeyDown(Key::MouseRight));
    m_play->scene.setViewOverride(m_freeCamera.active() ? std::optional{m_freeCamera.camera()}
                                                        : std::nullopt);
}

Vec3 Gauntlet::consoleFlight(s32 slot, f64 seconds, bool consoleOpen) {
    NoclipMotion& flight = m_noclip[static_cast<usize>(slot)];
    // The slot flies the seat it plays in the scene being stepped: online the seat its device
    // holds, offline the player of that number.
    const PlayScene* scene = consoleScene();
    bool flying = false;
    if (scene != nullptr) {
        const std::optional<u8> seat =
            m_online ? m_online->consoleSeat(slot) : std::optional{static_cast<u8>(slot)};
        flying = seat.has_value() && scene->cheats(*seat).noclip;
    }
    // No flight while its seat's noclip is off or the console is down.
    if (!flying || consoleOpen) {
        flight.reset();
        return Vec3{0.0f};
    }
    // Noclip flies a character as the camera looks, the console's keys left alone.
    const MenuInputSource source = playerInputSource(input(), m_config, slot);
    const NoclipMotion::Wish wish = NoclipMotion::wish(
        input(), playBindings(m_config, slot), source.keyboard, source.pad, scene->viewCamera());
    return flight.fly(wish, static_cast<f32>(seconds));
}

void Gauntlet::resetConsoleFlights() {
    for (NoclipMotion& flight : m_noclip) {
        flight.reset();
    }
}

/** The player the console speaks for: the one at the keyboard, else the first in. */
s32 Gauntlet::consolePlayer() {
    s32 first = -1;
    if (m_online) {
        for (s32 device = 0; device < PlayScene::kPlayerCount; ++device) {
            const auto seat = m_online->consoleSeat(device);
            if (!seat || !consoleScene() || !consoleScene()->actor(*seat)) {
                continue;
            }
            if (first < 0) {
                first = *seat;
            }
            if (playerInputSource(input(), m_config, device).keyboard) {
                return *seat;
            }
        }
        return first;
    }
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (m_play->scene.actor(player) == nullptr) {
            continue;
        }
        if (first < 0) {
            first = player;
        }
        if (playerInputSource(input(), m_config, player).keyboard) {
            return player;
        }
    }
    return first;
}

} // namespace gdl::game
