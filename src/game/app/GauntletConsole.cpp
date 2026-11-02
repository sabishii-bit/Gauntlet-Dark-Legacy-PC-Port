#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/assets/ModelSet.h"
#include "engine/assets/ObjectMaterial.h"
#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/platform/Paths.h"
#include "expansions/n64/level/LegendsUnlocks.h"
#include "expansions/shared/scenes/LegendsLevel.h"

#include "game/app/Gauntlet.h"
#include "game/characters/ClassData.h"
#include "game/characters/Progression.h"
#include "game/console/ConsoleCommands.h"
#include "game/critters/CombatantKind.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/Generators.h"
#include "game/level/PlacedItems.h"
#include "game/level/TowerAccess.h"
#include "game/menu/MenuInput.h"
#include "game/players/ItemPickup.h"
#include "game/players/NoclipMotion.h"
#include "game/scenes/SceneSelection.h"

namespace gdl::game {

void Gauntlet::setUpConsole() {
    if (!m_console.load(renderDevice(), m_options.unpackedDirectory)) {
        log::warn("Developer console: the 8x8 font is missing; its screen will be blank");
    }
    m_console.print(std::format("CGaunt++ {}", m_buildLabel.text()));
    m_console.print("Type help for the commands.");
    m_console.setPolicy(
        [this](std::string_view name) {
            if (name == "party_count" || name == "activate" || name == "unlock" || name == "lock") {
                return !m_online && !m_netplay; // the level's own state: offline only
            }
            if (m_legendsPreview.isOpen()) {
                return name == "help" || name == "goto";
            }
            if (name == "console_permissions") {
                return m_online && m_online->consoleHost();
            }
            return !m_online || m_online->consoleAllowed();
        },
        [this](DevConsole& console, std::string_view name, DevConsole::Arguments arguments) {
            return dispatchOnlineConsole(console, name, arguments);
        });
    m_console.addCommand(
        "console_permissions", "0 or 1: host allows guests to use the console",
        [this](DevConsole& console, DevConsole::Arguments arguments) {
            if (arguments.size() != 1 || (arguments[0] != "0" && arguments[0] != "1")) {
                console.print("Usage: console_permissions 0 or 1");
            } else if (m_online && m_online->consoleHost() &&
                       m_online->setConsolePermissions(arguments[0] == "1")) {
                console.print(arguments[0] == "1" ? "Guest consoles enabled"
                                                  : "Guest consoles disabled");
            } else {
                console.print("Only the online host can change console permissions.");
            }
        });
    m_console.addCommand("debug_info", "shows or hides the level debug overlay",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             m_levelDebug.toggle();
                             console.print(m_levelDebug.visible() ? "Level debug overlay on"
                                                                  : "Level debug overlay off");
                             if (m_levelDebug.visible() && consoleScene()) {
                                 for (const std::string& line : levelDebugLines()) {
                                     log::info("Level debug: {}", line);
                                 }
                             }
                         });
    m_console.addCommand(
        "list", "maps, items or inventory: what there is",
        [this](DevConsole& console, DevConsole::Arguments arguments) {
            consoleList(console, arguments);
        },
        wordsCompleter({"maps", "items", "inventory", "enemies", "critters", "generators",
                        "objects", "switches", "characters"}));
    m_console.addCommand(
        "activate", "$SELECTED, switch#N, an id or a name: fires that switch as if stood on",
        [this](DevConsole& console, DevConsole::Arguments arguments) {
            consoleActivate(console, arguments);
        },
        [this](DevConsole::Arguments arguments) { return activateWords(arguments); });
    m_console.addCommand(
        "unlock", "a door or chest without a key, or character <code> for the select screen",
        [this](DevConsole& console, DevConsole::Arguments arguments) {
            consoleLock(console, arguments, true);
        },
        [this](DevConsole::Arguments arguments) { return lockWords(arguments); });
    m_console.addCommand(
        "lock", "a door shut or a chest wanting a key again, or character <code> hidden again",
        [this](DevConsole& console, DevConsole::Arguments arguments) {
            consoleLock(console, arguments, false);
        },
        [this](DevConsole::Arguments arguments) { return lockWords(arguments); });
    m_console.addCommand(
        "goto", "<map> [x y z], x y z or <legends map> [preview]: brings the party there",
        [this](DevConsole& console, DevConsole::Arguments arguments) {
            consoleGoto(console, arguments);
        },
        [this](DevConsole::Arguments arguments) { return gotoWords(arguments); });
    m_console.addCommand(
        "party_count", "<1-4> or off: the party size the level sets itself up for",
        [this](DevConsole& console, DevConsole::Arguments arguments) {
            consolePartyCount(console, arguments);
        },
        wordsCompleter({"1", "2", "3", "4", "off"}));
    m_console.addCommand("unlock_tower", "opens every wing and portal of the tower",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleUnlockTower(console);
                         });
    m_console.addCommand("get", "<item> [amount], exp <points> or level <level>: for you",
                         [this](DevConsole& console, DevConsole::Arguments arguments) {
                             consoleGet(console, arguments);
                         });
    m_console.addCommand("spawn",
                         "stands enemies, great ones, generators, pickups or objects before you; "
                         "spawn alone says how",
                         [this](DevConsole& console, DevConsole::Arguments arguments) {
                             consoleSpawn(console, arguments);
                         });
    m_console.addCommand("suicide", "fells everyone standing",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleSuicide(console);
                         });
    m_console.addCommand("notarget", "the enemies stop seeing you, or see you again",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleToggleCheat(console, &PlayScene::ConsoleCheats::noTarget,
                                                "The enemies no longer see you",
                                                "The enemies see you again");
                         });
    m_console.addCommand("god", "nothing harms you, or everything does again",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleToggleCheat(console, &PlayScene::ConsoleCheats::god,
                                                "God mode on", "God mode off");
                         });
    m_console.addCommand("hidehud", "hides or shows every HUD element",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleHideHud(console);
                         });
    m_console.addCommand("freecam", "lets the camera off the party to fly; you stand still",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleFreeCam(console);
                         });
    m_console.addCommand("noclip", "flies you through everything, or sets you down again",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleNoclip(console);
                         });
}

const PlayScene* Gauntlet::consoleScene() const {
    return m_online ? m_online->consoleScene()
                    : (m_play->scene.isOpen() ? &m_play->scene : nullptr);
}

const LevelWorld* Gauntlet::consoleWorld() const {
    return m_online ? m_online->consoleWorld()
                    : (m_play->scene.isOpen() ? &m_play->world : nullptr);
}

bool Gauntlet::dispatchOnlineConsole(DevConsole& console, std::string_view name,
                                     DevConsole::Arguments arguments) {
    if (!m_online || name == "console_permissions" || name == "help" || name == "list" ||
        name == "debug_info" || name == "hidehud" || name == "freecam") {
        return false;
    }
    std::string error;
    if (name == "get") {
        const auto gift = ConsoleCommands::parseGet(arguments, error);
        if (gift && gift->kind == ConsoleCommands::Gift::Kind::Info) {
            return false;
        }
        if (gift) {
            error.clear();
        }
    } else if (name == "goto") {
        if (const auto destination = ConsoleCommands::parseGoto(arguments, error)) {
            error = destination->offlineOnly() ? "Legends maps are offline only." : "";
        }
    } else if (name == "spawn") {
        if (ConsoleCommands::parseSpawn(arguments, error)) {
            error.clear();
        }
    } else if (!arguments.empty()) {
        error = std::format("Usage: {}", name);
    }
    if (!error.empty()) {
        console.print(error);
        return true;
    }
    const s32 seat = consolePlayer();
    std::string line(name);
    for (const auto& argument : arguments) {
        line += ' ';
        line += argument;
    }
    if (seat < 0 || !m_online->consoleCommand(static_cast<u8>(seat), std::move(line))) {
        console.print("Command not queued: permission denied, session busy, or request too long.");
    } else {
        console.print("Command requested for the shared game.");
    }
    return true;
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

void Gauntlet::consoleNoclip(DevConsole& console) {
    const s32 player = consolePlayer();
    if (player < 0) {
        console.print("Nobody is in.");
        return;
    }
    bool& flying = m_consoleCheats[static_cast<usize>(player)].noclip;
    flying = !flying;
    m_noclip[static_cast<usize>(player)].reset();
    console.print(flying ? std::format("Noclip on: {}", NoclipMotion::kHelp) : "Noclip off");
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
        const auto switches = consoleSwitches();
        if (id < 0 || static_cast<usize>(id) >= switches.size()) {
            lines.push_back(std::format("{} names no switch of {}", selection.token(),
                                        consoleWorld()->ref().name));
            break;
        }
        const ConsoleCommands::SwitchEntry& entry = switches[static_cast<usize>(id)];
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
        const std::string name =
            ConsoleCommands::instanceName(consoleWorld()->layout(), gate.instance);
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
        const std::string name =
            ConsoleCommands::instanceName(consoleWorld()->layout(), chest.instance);
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
                                    ConsoleCommands::pickupKindName(item.subtype), item.value,
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

void Gauntlet::consoleToggleCheat(DevConsole& console, bool PlayScene::ConsoleCheats::*flag,
                                  std::string_view on, std::string_view off) {
    const s32 player = consolePlayer();
    if (player < 0) {
        console.print("Nobody is in.");
        return;
    }
    bool& granted = m_consoleCheats[static_cast<usize>(player)].*flag;
    granted = !granted;
    console.print(granted ? on : off);
}

void Gauntlet::consoleHideHud(DevConsole& console) {
    m_hudHidden = !m_hudHidden;
    console.print(m_hudHidden ? "HUD hidden" : "HUD shown");
}

void Gauntlet::consoleFreeCam(DevConsole& console) {
    if (m_freeCamera.active()) {
        m_freeCamera.stop();
        console.print("Free camera off");
        return;
    }
    if (consoleScene() == nullptr) {
        console.print("Nothing is in play.");
        return;
    }
    m_freeCamera.start(consoleScene()->viewCamera());
    console.print(std::format("Free camera on: {}", FreeCamera::kHelp));
}

void Gauntlet::applyConsoleState(f64 deltaSeconds, bool consoleOpen) {
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        m_play->scene.setCheats(player, m_consoleCheats[static_cast<usize>(player)]);
    }
    m_play->scene.setHudHidden(m_hudHidden);
    if (m_freeCamera.active() && !consoleOpen) {
        m_freeCamera.update(input(), static_cast<f32>(deltaSeconds));
    }
    // While the free camera is turned by the right button, the pointer is hidden where
    // it is and shows there again when the button is let go.
    window().holdCursor(m_freeCamera.active() && !consoleOpen &&
                        input().isKeyDown(Key::MouseRight));
    // Noclip flies a character as the camera looks, the console's keys left alone.
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        NoclipMotion& flight = m_noclip[static_cast<usize>(player)];
        if (!m_consoleCheats[static_cast<usize>(player)].noclip) {
            continue;
        }
        if (consoleOpen) {
            flight.reset();
            continue;
        }
        const MenuInputSource source = playerInputSource(input(), m_config, player);
        const NoclipMotion::Wish wish =
            NoclipMotion::wish(input(), playBindings(m_config, player), source.keyboard, source.pad,
                               m_play->scene.viewCamera());
        const NoclipMotion::Step step = flight.advance(wish, static_cast<f32>(deltaSeconds));
        m_play->scene.noclipMove(player, step.offset, step.yaw);
    }
    m_play->scene.setViewOverride(m_freeCamera.active() ? std::optional{m_freeCamera.camera()}
                                                        : std::nullopt);
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

bool Gauntlet::consoleLevel(DevConsole& console, const std::optional<std::string>& map,
                            ConsoleLevel& level) {
    if (!map.has_value()) {
        const auto* world = consoleWorld();
        if (world == nullptr) {
            console.print("Nothing is in play.");
            return false;
        }
        level.name = world->ref().name;
        level.layout = &world->layout();
        if (const LevelInfo* info = world->level()) {
            level.roster = info->enemies;
        }
        return true;
    }
    const std::optional<LevelRef> ref =
        *map == "TOWER" ? std::optional{LevelRef::tower()} : m_levels.byName(*map);
    if (!ref.has_value()) {
        console.print(std::format("No map named {}; list maps names them", *map));
        return false;
    }
    if (!level.loaded.load(m_options.unpackedDirectory / ref->directory)) {
        console.print(std::format("{} is not unpacked", ref->name));
        return false;
    }
    level.name = ref->name;
    level.layout = &level.loaded;
    if (level.data.load(m_options.unpackedDirectory / ref->worldDataFile())) {
        if (const LevelInfo* info = level.data.level(ref->name)) {
            level.roster = info->enemies;
        }
    }
    return true;
}

void Gauntlet::consoleList(DevConsole& console, DevConsole::Arguments arguments) {
    using Listing = ConsoleCommands::Listing;
    const auto say = [&](const std::vector<std::string>& lines) {
        for (const std::string& line : lines) {
            console.print(line);
        }
    };
    const Listing listing = ConsoleCommands::listingOf(arguments);
    const std::optional<std::string> map = ConsoleCommands::listedMap(arguments);
    switch (listing) {
    case Listing::Maps:
        say(ConsoleCommands::mapLines(m_levels));
        // The Legends maps installed stand with them, each platform's on a line; nothing is
        // said of a platform without any.
        for (const LegendsPlatform platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
            std::vector<std::string> names;
            for (const LegendsMapEntry& map : legendsMapsOf(platform, true)) {
                names.push_back(map.name);
            }
            say(ConsoleCommands::legendsMapLines(platform, names));
        }
        return;
    case Listing::Inventory:
        if (const PlayerActor* actor =
                consoleScene() ? consoleScene()->actor(consolePlayer()) : nullptr) {
            say(ConsoleCommands::inventoryLines(actor->save()));
        } else {
            console.print("Nobody is in.");
        }
        return;
    case Listing::Unknown: console.print(ConsoleCommands::listUsage()); return;
    case Listing::Characters:
        if (m_saves.opened() || m_saves.open(m_config.saveDirectory(), m_config.save.slots)) {
            say(ConsoleCommands::characterLines(m_saves.classUnlocks()));
        } else {
            console.print("The saves cannot be opened, so what is unlocked is unknown.");
        }
        return;
    case Listing::Switches:
        if (!map.has_value() && consoleWorld() != nullptr) {
            console.print(std::format("In {}:", consoleWorld()->ref().name));
            say(ConsoleCommands::switchLines(consoleSwitches()));
            return;
        }
        break;
    case Listing::Items:
        if (!map.has_value()) {
            say(ConsoleCommands::itemLines());
        }
        break;
    case Listing::Enemies:
        if (!map.has_value()) {
            say(ConsoleCommands::enemyKindLines());
        }
        break;
    case Listing::Critters: say(ConsoleCommands::critterLines()); break;
    case Listing::Generators:
    case Listing::Objects: break;
    }
    ConsoleLevel level;
    if (!consoleLevel(console, map, level)) {
        return;
    }
    console.print(std::format("In {}:", level.name));
    switch (listing) {
    case Listing::Items: say(ConsoleCommands::pickupLines(*level.layout)); break;
    case Listing::Enemies:
        say(ConsoleCommands::rosterLines(level.roster));
        say(ConsoleCommands::generatorLines(*level.layout));
        say(ConsoleCommands::placedEnemyLines(*level.layout));
        break;
    case Listing::Critters: say(ConsoleCommands::placedCritterLines(*level.layout)); break;
    case Listing::Generators:
        say(ConsoleCommands::generatorLines(*level.layout));
        console.print("spawn generator <enemy> [tier] stands another");
        break;
    case Listing::Objects: say(ConsoleCommands::objectLines(*level.layout)); break;
    case Listing::Switches:
        say(ConsoleCommands::switchLines(ConsoleCommands::switchesOf(*level.layout)));
        break;
    case Listing::Maps:
    case Listing::Inventory:
    case Listing::Characters:
    case Listing::Unknown: break;
    }
}

std::vector<ConsoleCommands::SwitchEntry> Gauntlet::consoleSwitches() const {
    std::vector<ConsoleCommands::SwitchEntry> switches;
    const LevelWorld* world = consoleWorld();
    if (world == nullptr) {
        return switches;
    }
    const WorldLayout& layout = world->layout();
    const LevelTriggers& triggers = world->triggers();
    for (usize i = 0; i < triggers.size(); ++i) {
        const LevelTrigger& trigger = triggers.trigger(i);
        ConsoleCommands::SwitchEntry entry;
        entry.index = static_cast<s32>(i);
        entry.id = trigger.id;
        entry.name = ConsoleCommands::instanceName(layout, trigger.instance);
        if (trigger.target >= 0 && static_cast<usize>(trigger.target) < layout.objects().size()) {
            entry.target = layout.objects()[static_cast<usize>(trigger.target)].name;
        }
        entry.shootable = trigger.shootable;
        entry.chained = trigger.chained;
        entry.fired = trigger.fired;
        entry.enabled = trigger.enabled;
        switches.push_back(std::move(entry));
    }
    return switches;
}

std::vector<Gauntlet::ConsoleFixture> Gauntlet::consoleFixtures() const {
    std::vector<ConsoleFixture> fixtures;
    const PlayScene* scene = consoleScene();
    const LevelWorld* world = consoleWorld();
    if (scene == nullptr || world == nullptr) {
        return fixtures;
    }
    const WorldLayout& layout = world->layout();
    const LockedGates& gates = scene->gates();
    for (usize i = 0; i < gates.size(); ++i) {
        if (gates.gate(i).shown) {
            fixtures.push_back({SceneSelection::Kind::Gate,
                                i,
                                {std::format("gate#{}", i),
                                 ConsoleCommands::instanceName(layout, gates.gate(i).instance)}});
        }
    }
    const Chests& chests = scene->chests();
    for (usize i = 0; i < chests.size(); ++i) {
        if (chests.chest(i).shown && !chests.chest(i).gone) {
            fixtures.push_back({SceneSelection::Kind::Chest,
                                i,
                                {std::format("chest#{}", i),
                                 ConsoleCommands::instanceName(layout, chests.chest(i).instance)}});
        }
    }
    return fixtures;
}

std::vector<std::string> Gauntlet::activateWords(DevConsole::Arguments arguments) const {
    std::vector<std::string> words;
    if (arguments.size() > 1) {
        return words;
    }
    for (const ConsoleCommands::NamedEntry& entry :
         ConsoleCommands::switchEntries(consoleSwitches())) {
        words.push_back(entry.token);
        if (!entry.name.empty()) {
            words.push_back(entry.name);
        }
    }
    return words;
}

std::vector<std::string> Gauntlet::lockWords(DevConsole::Arguments arguments) const {
    std::vector<std::string> words;
    if (arguments.size() > 2) {
        return words;
    }
    if (arguments.size() == 2) {
        if (arguments[0] == "character" || arguments[0] == "CHARACTER") {
            for (s32 i = kStartingClassCount; i < kClassCount; ++i) {
                words.emplace_back(classCode(i));
            }
        }
        return words;
    }
    words.emplace_back("character");
    for (const ConsoleFixture& fixture : consoleFixtures()) {
        words.push_back(fixture.entry.token);
        if (!fixture.entry.name.empty()) {
            words.push_back(fixture.entry.name);
        }
    }
    return words;
}

void Gauntlet::consoleActivate(DevConsole& console, DevConsole::Arguments arguments) {
    if (arguments.size() != 1) {
        console.print(ConsoleCommands::activateUsage());
        return;
    }
    if (consoleScene() == nullptr || !m_play || !m_play->scene.isOpen()) {
        console.print("Nothing is in play.");
        return;
    }
    const auto switches = consoleSwitches();
    const auto entries = ConsoleCommands::switchEntries(switches);
    const std::vector<usize> found = ConsoleCommands::entriesNamed(entries, arguments[0]);
    if (found.empty()) {
        console.print(std::format("{} names no switch here; list switches names them, or click "
                                  "a pad and activate $SELECTED",
                                  arguments[0]));
        return;
    }
    if (found.size() > 1) {
        std::string tokens;
        for (const usize index : found) {
            tokens += tokens.empty() ? "" : ", ";
            tokens += entries[index].token;
        }
        console.print(std::format("{} names {} switches: {}; say which by its token", arguments[0],
                                  found.size(), tokens));
        return;
    }
    const ConsoleCommands::SwitchEntry& entry = switches[found[0]];
    if (!m_play->scene.pressSwitch(static_cast<usize>(entry.index))) {
        console.print(std::format("switch#{} {} is not for this party, or stands open for good",
                                  entry.index, entry.name));
        return;
    }
    console.print(entry.target.empty()
                      ? std::format("Pressed switch#{} {}", entry.index, entry.name)
                      : std::format("Pressed switch#{} {}: {} answers, the camera following",
                                    entry.index, entry.name, entry.target));
}

void Gauntlet::consoleLock(DevConsole& console, DevConsole::Arguments arguments, bool unlock) {
    const std::string_view verb = unlock ? "unlock" : "lock";
    std::string error;
    const auto request = ConsoleCommands::parseLockRequest(arguments, error, verb);
    if (!request.has_value()) {
        console.print(error);
        return;
    }
    if (request->character) {
        const auto index = classIndexOf(request->what);
        if (!index.has_value()) {
            console.print(
                std::format("No character is coded {}; list characters names them", request->what));
            return;
        }
        if (*index < kStartingClassCount) {
            console.print(std::format("{} is always there to choose", classCode(*index)));
            return;
        }
        if (!m_saves.opened() && !m_saves.open(m_config.saveDirectory(), m_config.save.slots)) {
            console.print("The saves cannot be opened.");
            return;
        }
        const auto mask = static_cast<u16>(1U << (*index - kStartingClassCount));
        const bool written = unlock ? m_saves.unlockClasses(mask) : m_saves.lockClasses(mask);
        if (!unlock && m_play && m_play->scene.isOpen()) {
            // The party in play carries the old reward bits; they would bring it back.
            for (s32 player = 0; player < 4; ++player) {
                if (CharacterSave* save = m_play->scene.liveSave(player)) {
                    save->classUnlock &= static_cast<u16>(~mask);
                }
            }
        }
        console.print(
            std::format("{} {}{}", classCode(*index),
                        unlock ? "unlocked: the select screen offers it"
                               : "locked: the select screen hides it again",
                        written ? "" : " (for this session; the profile was not written)"));
        return;
    }
    if (consoleScene() == nullptr || !m_play || !m_play->scene.isOpen()) {
        console.print("Nothing is in play.");
        return;
    }
    const auto fixtures = consoleFixtures();
    std::vector<ConsoleCommands::NamedEntry> entries;
    entries.reserve(fixtures.size());
    for (const ConsoleFixture& fixture : fixtures) {
        entries.push_back(fixture.entry);
    }
    const std::vector<usize> found = ConsoleCommands::entriesNamed(entries, request->what);
    if (found.empty()) {
        console.print(std::format("{} names no door or chest here; gate#N, chest#N, a name or "
                                  "$SELECTED after a click does",
                                  request->what));
        return;
    }
    if (found.size() > 1) {
        std::string tokens;
        for (const usize index : found) {
            tokens += tokens.empty() ? "" : ", ";
            tokens += entries[index].token;
        }
        console.print(std::format("{} names {} things: {}; say which by its token", request->what,
                                  found.size(), tokens));
        return;
    }
    const ConsoleFixture& fixture = fixtures[found[0]];
    const std::string label = fixture.entry.name.empty()
                                  ? fixture.entry.token
                                  : std::format("{} {}", fixture.entry.token, fixture.entry.name);
    if (fixture.kind == SceneSelection::Kind::Gate) {
        const bool done = unlock ? m_play->scene.unlockGate(fixture.index)
                                 : m_play->scene.lockGate(fixture.index);
        if (!done) {
            console.print(std::format("{} is {} already", label, unlock ? "open" : "shut"));
        } else {
            console.print(unlock ? std::format("{} unlocked: it swings open", label)
                                 : std::format("{} locked: shut until a key is spent", label));
        }
        return;
    }
    if (!m_play->scene.setChestLocked(fixture.index, !unlock)) {
        console.print(std::format("{} is open already", label));
        return;
    }
    console.print(unlock ? std::format("{} unlocked: it opens without a key", label)
                         : std::format("{} locked: a key opens it", label));
}

void Gauntlet::consoleBorrowVoices(DevConsole& console, const LevelRef& level) {
    WorldData data;
    const LevelInfo* info = data.load(m_options.unpackedDirectory / level.worldDataFile())
                                ? data.level(level.name)
                                : nullptr;
    if (info == nullptr ||
        !m_play->scene.borrowOpponentSounds(*info, data.audio(info->audioIndex))) {
        console.print(std::format("({} lends no voices)", level.name));
    }
}

void Gauntlet::consoleSpawn(DevConsole& console, DevConsole::Arguments arguments) {
    constexpr f32 kAhead = 6.0f; ///< units before the character
    constexpr f32 kApart = 3.0f; ///< between several
    constexpr f32 kProbeAbove = 3.0f;
    constexpr f32 kProbeBelow = 10.0f;
    const PlayerActor* actor = m_play->scene.actor(consolePlayer());
    if (actor == nullptr) {
        console.print("Nobody is in to spawn before.");
        return;
    }
    std::string error;
    const auto spawn = ConsoleCommands::parseSpawn(arguments, error);
    if (!spawn.has_value()) {
        console.print(error);
        return;
    }
    // The map named, when one is: where a visitor's voices, costume or object come from.
    std::optional<LevelRef> from;
    if (spawn->map.has_value()) {
        from = *spawn->map == "TOWER" ? std::optional{LevelRef::tower()}
                                      : m_levels.byName(*spawn->map);
        if (!from.has_value()) {
            console.print(std::format("No map named {}; list maps names them", *spawn->map));
            return;
        }
    }
    const Vec3 facing = actor->facing();
    const f32 yawToPlayer = actor->yaw() + kPi;
    const auto spotAt = [&](s32 index) {
        Vec3 spot = actor->position() + facing * (kAhead + kApart * static_cast<f32>(index));
        if (const auto floor = m_play->world.collision().floorAt(
                spot + Vec3{0.0f, kProbeAbove, 0.0f}, kProbeAbove, kProbeBelow)) {
            spot.y = floor->y;
        }
        return spot;
    };
    PlayScene& scene = m_play->scene;
    switch (spawn->kind) {
    case ConsoleCommands::Spawn::Kind::Enemy: {
        if (from.has_value()) {
            consoleBorrowVoices(console, *from);
        }
        s32 stood = 0;
        const std::vector<EnemyView> views = scene.opponents().views(scene.participants());
        for (s32 i = 0; i < spawn->count; ++i) {
            if (scene.enemies().loadKind(spawn->enemyKind) &&
                scene.enemies()
                    .spawn(EnemySpawn{.kind = spawn->enemyKind,
                                      .tier = spawn->tier,
                                      .position = spotAt(i),
                                      .direction = -facing,
                                      .placed = true},
                           views)
                    .has_value()) {
                ++stood;
            }
        }
        console.print(
            stood > 0 ? std::format("Spawned {} {} at tier {}", stood, spawn->name, spawn->tier)
                      : std::format("No room for {} here, or its kind is missing", spawn->name));
        break;
    }
    case ConsoleCommands::Spawn::Kind::Critter: {
        const char realm =
            from.has_value() && !from->name.empty()
                ? static_cast<char>(std::toupper(static_cast<unsigned char>(from->name.front())))
                : '\0';
        std::optional<s32> id;
        switch (spawn->critter) {
        case CombatantKind::Golem:
            id = scene.critters().spawnGolem(spotAt(0), yawToPlayer, realm);
            break;
        case CombatantKind::General:
            id = scene.critters().spawnGeneral(spotAt(0), yawToPlayer, 0.0f, realm);
            break;
        case CombatantKind::Gargoyle:
            id = scene.critters().spawnGargoyle(spotAt(0), yawToPlayer, spawn->form, realm);
            break;
        case CombatantKind::Unknown:
        case CombatantKind::Boss: break;
        }
        const std::string& what = spawn->form.empty() ? spawn->name : spawn->form;
        console.print(id.has_value()
                          ? std::format("Spawned a {}", what)
                          : std::format("No room for a {} here, or its kind is missing", what));
        break;
    }
    case ConsoleCommands::Spawn::Kind::Generator: {
        if (from.has_value()) {
            consoleBorrowVoices(console, *from);
        }
        s32 kind = spawn->enemyKind;
        ItemArchive* realmItems = nullptr;
        const ItemInfo* record = nullptr;
        ConsoleLevel source;
        if (spawn->brood) {
            // A mixed brood is its realm's: its bodies and record come from that realm's
            // level, the one in play when it is one of them.
            const s32 realm = from.has_value() ? from->realmId : m_play->world.ref().realmId;
            if (realm != TowerAccess::kTempleWorld && realm != TowerAccess::kUnderworldWorld) {
                console.print("A mixed brood is the temple's (E) or the underworld's (F); name "
                              "a map of either");
                break;
            }
            kind = realm == TowerAccess::kTempleWorld ? Generators::kTempleBrood
                                                      : Generators::kHellBrood;
            if (from.has_value()) {
                realmItems = scene.borrowItems(*from);
                if (realmItems == nullptr) {
                    console.print(std::format("{} lends no item archive", from->name));
                    break;
                }
                if (consoleLevel(console, spawn->map, source)) {
                    record = ConsoleCommands::generatorRecord(*source.layout, "SPECIAL");
                }
            }
        }
        const auto id = scene.generators().spawn(renderDevice(), scene.enemies(), kind, spawn->tier,
                                                 spotAt(0), yawToPlayer, realmItems, record);
        console.print(id.has_value()
                          ? std::format("Stood a tier {} {} generator", spawn->tier, spawn->name)
                          : std::format("No {} generator could stand here", spawn->name));
        break;
    }
    case ConsoleCommands::Spawn::Kind::Item:
        if (from.has_value()) {
            console.print("Pickups come from the level in play alone; name one without a map");
        } else if (m_play->world.placeItem(renderDevice(), spawn->name, spotAt(0))) {
            console.print(std::format("Dropped {}", spawn->name));
        } else {
            console.print(
                std::format("No pickup record named {} here; list items names them", spawn->name));
        }
        break;
    case ConsoleCommands::Spawn::Kind::Object: {
        const LevelRef source = from.has_value() ? *from : m_play->world.ref();
        if (scene.spawnObject(source, spawn->name, spotAt(0), yawToPlayer)) {
            console.print(std::format("Stood {} of {}", spawn->name, source.name));
        } else {
            console.print(std::format("Nothing named {} in {}; list objects {} names what there is",
                                      spawn->name, source.name, source.name));
        }
        break;
    }
    case ConsoleCommands::Spawn::Kind::Thing: {
        if (!from.has_value() && m_play->world.placeItem(renderDevice(), spawn->name, spotAt(0))) {
            console.print(std::format("Dropped {}", spawn->name));
            break;
        }
        const LevelRef source = from.has_value() ? *from : m_play->world.ref();
        if (scene.spawnObject(source, spawn->name, spotAt(0), yawToPlayer)) {
            console.print(std::format("Stood {} of {}", spawn->name, source.name));
        } else if (from.has_value()) {
            console.print(std::format("Nothing named {} in {}; list objects {} names what there is",
                                      spawn->name, from->name, from->name));
        } else {
            console.print(std::format("Nothing named {} here; list items, objects, enemies or "
                                      "critters name what there is",
                                      spawn->name));
        }
        break;
    }
    }
}

void Gauntlet::consoleSuicide(DevConsole& console) {
    if (m_play->world.isTower()) {
        console.print("Nobody falls in the tower.");
        return;
    }
    s32 fallen = 0;
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (m_play->scene.kill(player)) {
            ++fallen;
        }
    }
    console.print(fallen > 0 ? std::format("{} fallen", fallen) : "Nobody was standing.");
}

/** The words `goto` takes, for completion: the levels, the tower, the Legends maps installed
 * and the way back from one. */
std::vector<std::string> Gauntlet::gotoWords(DevConsole::Arguments arguments) const {
    if (arguments.size() > 1) {
        return arguments.size() == 2 ? std::vector<std::string>{"preview"}
                                     : std::vector<std::string>{};
    }
    std::vector<std::string> words{"tower", "back"};
    for (const LevelCatalog::Realm& realm : m_levels.realms()) {
        words.insert(words.end(), realm.levels.begin(), realm.levels.end());
    }
    for (const LegendsPlatform platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        for (const LegendsMapEntry& map : legendsMapsOf(platform)) {
            words.push_back(map.name);
        }
    }
    return words;
}

void Gauntlet::consolePartyCount(DevConsole& console, DevConsole::Arguments arguments) {
    const auto describe = [&] {
        return m_partyCountOverride.has_value()
                   ? std::format("The level is set up for a party of {}", *m_partyCountOverride)
                   : std::string("The level is set up for the players present");
    };
    if (arguments.empty()) {
        console.print(describe());
        return;
    }
    std::string error;
    const auto parsed = ConsoleCommands::parsePartyCount(arguments, error);
    if (!parsed.has_value()) {
        console.print(error);
        return;
    }
    m_partyCountOverride = *parsed;
    if (m_play->scene.isOpen()) {
        m_play->scene.setPartyCountOverride(m_partyCountOverride);
    }
    console.print(describe());
}

void Gauntlet::consoleGoto(DevConsole& console, DevConsole::Arguments arguments) {
    std::string error;
    const auto destination = ConsoleCommands::parseGoto(arguments, error);
    if (!destination.has_value()) {
        console.print(error);
        return;
    }
    if (destination->back) {
        consoleLegendsClose(console);
        return;
    }
    if (destination->legends.has_value()) {
        if (m_online || m_netplay) {
            console.print("Legends maps are offline only.");
            return;
        }
        const std::string& name = *destination->legends;
        const bool path = name.find_first_of("/\\") != std::string::npos;
        if (path && !destination->platform.has_value()) {
            console.print("Name the platform with a path: goto n64|dreamcast <path>");
            return;
        }
        const auto found =
            path ? std::optional{std::pair{*destination->platform, paths::fromUtf8(name)}}
                 : findLegendsMap(name, destination->platform);
        if (!found.has_value()) {
            // A word that is neither a level nor an installed Legends map, as before the
            // Legends maps were taken: the levels are what list maps names first.
            console.print(
                std::format("No level or Legends map named {}; list maps names them", name));
            return;
        }
        consoleLegendsOpen(console, found->first, found->second, !destination->preview);
        return;
    }
    if (m_play->scene.leaving()) {
        console.print("The party is already on its way.");
        return;
    }
    if (!destination->level.has_value()) {
        const Vec3& at = *destination->position;
        if (m_play->scene.relocate(at)) {
            console.print(std::format("Moved to {:.1f} {:.1f} {:.1f}", at.x, at.y, at.z));
        } else {
            console.print("Nobody is standing to move.");
        }
        return;
    }
    const std::optional<LevelRef> level = *destination->level == "TOWER"
                                              ? std::optional{LevelRef::tower()}
                                              : m_levels.byName(*destination->level);
    if (!level.has_value()) {
        console.print(std::format("No map named {}; list maps names them", *destination->level));
        return;
    }
    console.print(std::format("Going to {} ({})", level->name, level->title));
    consoleTravel(*level, destination->position, std::nullopt);
}

/** Takes the party to `level` through the loading picture, as an exit portal would, standing
 * it at `position` facing `yaw` when given. The scene closes here: the tower update that
 * runs the command must go no further this frame. */
void Gauntlet::consoleTravel(const LevelRef& level, std::optional<Vec3> position,
                             std::optional<f32> yaw) {
    Journey journey;
    journey.destination = level;
    journey.party = m_play->scene.party();
    journey.options.welcome = false;
    journey.options.position = position;
    journey.options.yaw = yaw;
    m_freeCamera.stop(); // its place means nothing in another level
    m_play->scene.close();
    m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
    m_loadingPicture.cover();
    m_journey = std::move(journey);
}

void Gauntlet::consoleUnlockTower(DevConsole& console) {
    if (auto* native = dynamic_cast<LegendsLevel*>(m_play->world.imported())) {
        if (native->platform() != LegendsPlatform::N64) {
            console.print("Native Dreamcast tower unlocking is not supported yet.");
            return;
        }
        try {
            const auto root = native->directory().parent_path().parent_path();
            const auto program = AssetLocator{root}.find("_program/game.bin");
            if (!program) {
                console.print("The native N64 unlock table is unavailable.");
                return;
            }
            const auto unlocks = LegendsN64Unlocks::decode(readFile(*program));
            s32 changed = 0;
            for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
                if (auto* save = m_play->scene.liveSave(player)) {
                    if (unlocks.unlockAll(save->progress().legendsProgress(LegendsPlatform::N64))) {
                        ++changed;
                    }
                }
            }
            // Native gates observe the live union on the next gameplay tick.
            // Never reopen the Dark Legacy tower or mutate its crystal/relic bits.
            console.print(std::format("The N64 tower stands open to {} character{}.", changed,
                                      changed == 1 ? "" : "s"));
        } catch (const std::exception& error) {
            console.print(std::format("Native tower unlock failed: {}", error.what()));
        }
        return;
    }
    s32 opened = 0;
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (CharacterSave* save = m_play->scene.liveSave(player)) {
            ConsoleCommands::unlockTower(*save);
            ++opened;
        }
    }
    if (opened == 0) {
        console.print("Nobody is in to open it for.");
        return;
    }
    console.print(
        std::format("The tower stands open to {} character{}.", opened, opened == 1 ? "" : "s"));
    if (m_play->world.isTower()) {
        // The portals read the party's progress as the tower opens: open it again, where
        // the party stands.
        const PlayerActor* actor = m_play->scene.actor(consolePlayer());
        consoleTravel(LevelRef::tower(),
                      actor != nullptr ? std::optional{actor->position()} : std::nullopt,
                      actor != nullptr ? std::optional{actor->yaw()} : std::nullopt);
    }
}

void Gauntlet::consoleGet(DevConsole& console, DevConsole::Arguments arguments) {
    std::string error;
    const auto gift = ConsoleCommands::parseGet(arguments, error);
    if (!gift.has_value()) {
        console.print(error);
        return;
    }
    if (gift->kind == ConsoleCommands::Gift::Kind::Info) {
        const std::optional<SceneSelection> what =
            gift->name.empty() ? (consoleScene() ? consoleScene()->selection() : std::nullopt)
                               : SceneSelection::parse(gift->name);
        if (!what.has_value()) {
            console.print(gift->name.empty()
                              ? "Nothing is selected: click something in the view while the "
                                "console is down, then get info $SELECTED"
                              : std::format("{} names nothing; $SELECTED or a token such as "
                                            "object#12 or enemy#3 does",
                                            gift->name));
            return;
        }
        for (const std::string& line : consoleDescribe(*what)) {
            console.print(line);
        }
        return;
    }
    const s32 player = consolePlayer();
    CharacterSave* save = m_play->scene.liveSave(player);
    if (save == nullptr) {
        console.print("Nobody is in to give it to.");
        return;
    }
    switch (gift->kind) {
    case ConsoleCommands::Gift::Kind::Experience:
        m_play->scene.awardExperience(player, gift->amount, false);
        m_play->scene.restat(player);
        console.print(std::format("{} experience: level {}", gift->amount,
                                  experienceLevel(save->experience())));
        break;
    case ConsoleCommands::Gift::Kind::Level:
        save->progress().experience = levelExperience(gift->amount);
        m_play->scene.restat(player);
        console.print(std::format("Level {}", gift->amount));
        break;
    case ConsoleCommands::Gift::Kind::Item:
        // At one times the strength: the seconds asked for are the seconds given.
        console.print(ConsoleCommands::report(*gift, takeItem(*save, gift->offer, 1.0f, true)));
        break;
    }
}

} // namespace gdl::game
