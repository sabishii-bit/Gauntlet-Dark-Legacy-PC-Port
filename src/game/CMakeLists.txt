# Everything except main() lives in a library so the tests can link it.
# Modules, lowest first: config (settings), players (class data, progression, saves, and the
# characters as they stand in a level), world (the levels the game plays in and their
# cameras), menu (widgets), screens (full screens and what they receive), app (the driver and
# attract flow). A module only includes those below it.
add_library(game STATIC
    app/Gauntlet.h
    app/Gauntlet.cpp
    app/CommandLine.h
    app/CommandLine.cpp
    app/Scenario.h
    app/Scenario.cpp
    app/AttractSequencer.h
    app/AttractSequencer.cpp
    config/GameConfig.h
    config/GameConfig.cpp
    players/ClassData.h
    players/ClassData.cpp
    players/Progression.h
    players/Progression.cpp
    players/CharacterSave.h
    players/CharacterSave.cpp
    players/TurboMeter.h
    players/TurboMeter.cpp
    players/TurboMove.cpp
    players/Party.h
    players/Party.cpp
    players/Inventory.h
    players/Inventory.cpp
    players/Relics.h
    players/Relics.cpp
    players/ItemPickup.h
    players/ItemPickup.cpp
    players/PowerupEffects.h
    players/PowerupEffects.cpp
    players/PlayerControls.h
    players/PlayerControls.cpp
    players/PlayerActor.h
    players/PlayerActor.cpp
    players/PlayerCapture.h
    players/PlayerCapture.cpp
    players/PlayerAnimator.h
    players/PlayerAnimator.cpp
    players/PlayerImpact.h
    players/PlayerImpact.cpp
    world/LevelCatalog.h
    world/LevelCatalog.cpp
    world/ItemFigure.h
    world/ItemFigure.cpp
    world/MoveStrikes.h
    world/MoveStrikes.cpp
    world/Breakables.h
    world/Breakables.cpp
    world/SafeRocks.h
    world/SafeRocks.cpp
    players/LevelWatch.cpp
    enemies/EnemyKinds.cpp
    enemies/EnemyAnimator.cpp
    enemies/EnemyMind.cpp
    enemies/EnemyMissiles.cpp
    enemies/Enemies.cpp
    enemies/Generators.cpp
    enemies/CritterData.cpp
    enemies/CritterMovement.cpp
    enemies/CombatantBreath.cpp
    enemies/CombatantProjectile.cpp
    world/CombatantProjectiles.cpp
    enemies/Critters.cpp
    enemies/Combatant.cpp
    enemies/MoveDefinition.cpp
    enemies/CombatantAssets.cpp
    enemies/CombatantMoves.cpp
    enemies/CombatantMotion.cpp
    enemies/CombatantAttacks.cpp
    enemies/CombatantGrabs.cpp
    enemies/CombatantView.cpp
    enemies/Golem.cpp
    enemies/General.cpp
    enemies/Gargoyle.cpp
    enemies/BossDefinition.cpp
    enemies/CombatantPatterns.cpp
    enemies/CritterArea.cpp
    enemies/CombatantAreas.cpp
    enemies/BossCoins.cpp
    enemies/SkorneRelics.cpp
    enemies/Bosses.cpp
    enemies/LegendItems.cpp
    world/Chests.h
    world/Chests.cpp
    world/LockedGates.h
    world/LockedGates.cpp
    world/Traps.h
    world/Traps.cpp
    world/ExitPortals.h
    world/ExitPortals.cpp
    world/LevelWorld.h
    world/LevelWorld.cpp
    world/TowerCamera.h
    world/TowerCamera.cpp
    world/BossCamera.h
    world/BossCamera.cpp
    world/CameraShake.h
    world/CameraShake.cpp
    world/StartCamera.h
    world/StartCamera.cpp
    world/SumnerFigure.h
    world/SumnerFigure.cpp
    world/SumnerHints.h
    world/SumnerHints.cpp
    world/PlayerMissiles.h
    world/PlayerMissiles.cpp
    world/TargetAssist.cpp
    world/PlayerFigure.cpp
    world/PlayerFamiliar.cpp
    world/CameraMovementLimit.cpp
    world/SkorneArena.cpp
    screens/TowerPromotion.cpp
    screens/PlayScenePromotion.cpp
    world/PlayerArsenal.cpp
    world/LevelSoundscape.cpp
    world/EffectTrees.h
    world/EffectTrees.cpp
    world/PlacedItems.h
    world/PlacedItems.cpp
    world/AmbientSounds.h
    world/AmbientSounds.cpp
    world/LevelTriggers.h
    world/LevelTriggers.cpp
    menu/ScrollBox.h
    menu/ScrollBox.cpp
    menu/MenuInput.h
    menu/MenuInput.cpp
    menu/OptionMenu.h
    menu/OptionMenu.cpp
    menu/HintMenu.h
    menu/HintMenu.cpp
    menu/BurnDialogueScroll.h
    menu/BurnDialogueScroll.cpp
    menu/NameEntry.h
    menu/NameEntry.cpp
    screens/GameContext.h
    screens/TitleScene.h
    screens/TitleScene.cpp
    screens/MovieScene.h
    screens/MovieScene.cpp
    screens/SmokeTestScene.h
    screens/SmokeTestScene.cpp
    screens/SelectLane.h
    screens/SelectLane.cpp
    screens/StatusBox.h
    screens/StatusBox.cpp
    screens/PowerupSelector.h
    screens/PowerupSelector.cpp
    screens/HelpMessages.h
    screens/HelpMessages.cpp
    screens/TransitionScreen.h
    screens/TransitionScreen.cpp
    screens/PickupHud.h
    screens/PickupHud.cpp
    screens/BossMeter.h
    screens/BossMeter.cpp
    screens/BossVictory.h
    screens/BossVictory.cpp
    screens/BossVictoryPresentation.cpp
    screens/BossSequence.cpp
    screens/LevelArrivalPresentation.cpp
    screens/LevelMessages.cpp
    screens/SumnerVisit.cpp
    screens/PlayerSelectScene.h
    screens/PlayerSelectScene.cpp
    screens/LegendPresentation.h
    screens/LegendPresentation.cpp
    screens/PlayScene.h
    screens/PartyMotion.cpp
    screens/PartyHud.cpp
    screens/LevelFixtures.cpp
    screens/LevelOpponents.cpp
    screens/PlayerAttacks.cpp
    screens/PlayerHealth.cpp
    screens/PlayScene.cpp)

add_library(gdl::game ALIAS game)

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(game PUBLIC gdl::engine PRIVATE nlohmann_json::nlohmann_json)
gdl_apply_compiler_options(game)
set_target_properties(game PROPERTIES FOLDER "game")

add_executable(gauntlet main.cpp)

target_link_libraries(gauntlet PRIVATE gdl::game)

# On Windows the executable carries the memory-card icon from the disc, which gdlunpack
# converts while building; nothing from the assets is checked in.
get_filename_component(GDL_DISC_DIR "${GDL_ASSET_DIR}" DIRECTORY)
set(GDL_CARD_ICON "${GDL_DISC_DIR}/carddemo/icon.tpl")
if(WIN32 AND EXISTS "${GDL_CARD_ICON}")
    enable_language(RC)
    set(GDL_ICON_FILE "${CMAKE_BINARY_DIR}/icon/carddemo/icon.ico")
    add_custom_command(
        OUTPUT "${GDL_ICON_FILE}"
        COMMAND gdlunpack "${GDL_ASSET_DIR}" "${CMAKE_BINARY_DIR}/icon" --only carddemo
        DEPENDS gdlunpack "${GDL_CARD_ICON}"
        COMMENT "Converting the memory-card icon for the executable"
        VERBATIM)
    configure_file(app/gauntlet.rc.in "${CMAKE_BINARY_DIR}/gauntlet.rc" @ONLY)
    set_source_files_properties("${CMAKE_BINARY_DIR}/gauntlet.rc" PROPERTIES
        OBJECT_DEPENDS "${GDL_ICON_FILE}")
    target_sources(gauntlet PRIVATE "${CMAKE_BINARY_DIR}/gauntlet.rc" "${GDL_ICON_FILE}")
endif()

target_compile_definitions(gauntlet PRIVATE
    GDL_DEFAULT_ASSET_DIR="${GDL_ASSET_DIR}"
    GDL_DEFAULT_UNPACKED_DIR="${GDL_UNPACKED_DIR}"
    GDL_DEFAULT_DATA_DIR="${GDL_DATA_DIR}")

gdl_apply_compiler_options(gauntlet)

set_target_properties(gauntlet PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "game")

gdl_add_shaders(gauntlet
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/shaders"
    SOURCES
        ${CMAKE_SOURCE_DIR}/shaders/immediate.vert
        ${CMAKE_SOURCE_DIR}/shaders/immediate.frag)
