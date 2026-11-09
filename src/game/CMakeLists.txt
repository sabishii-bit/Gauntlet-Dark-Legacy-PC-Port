# Everything except main() lives in a library so the tests can link it.
# Modules by subject, lowest first: config (settings), widgets (what every screen reads and
# draws with: menu input, fonts, option lists, scrolls), combat (damage rules), mechanics (the
# seam additive gameplay mechanics answer through, and the catalog of them), characters (class
# data, saves, inventory, progression), players (a character's body and controls), camera,
# audio (what a level sounds like), effects, level (the levels and what stands in them), party
# (the party in a level, frame by frame), hud (what the party sees in play: status boxes,
# messages, meters), enemies (the swarm and the level's opponents), critters, bosses, tower
# (between levels, with Sumner and his scrolls), shop (the shop and the inventory panel),
# context (GameServices: the settings, text, sound and files every screen reads from the game),
# menu (the title, pause, save and settings menus and the name picker), rollback (commands,
# timelines and checkpoints), netplay (rooms, sessions and keeping the peers in step), console
# (the developer console, its command objects and their parsers), scenes (the full screens),
# online (an online game's stages, over the scenes, and its lobby menu), app (the driver and
# attract flow). A module includes only those below it; app owns the input boundary.
add_library(game STATIC
    netplay/AssetDigest.cpp
    online/NetplayMenu.cpp
    netplay/HostedRoom.cpp
    rollback/InputCommand.cpp
    rollback/InputTimeline.cpp
    rollback/RollbackInputs.cpp
    rollback/InputJournal.cpp
    rollback/BitSet8.h
    netplay/RollbackExchange.cpp
    rollback/FrameEventLedger.cpp
    audio/SceneSoundEvents.cpp
    audio/AnnouncementSubject.h
    level/NativeStateResources.cpp
    netplay/NativeAgreement.cpp
    rollback/ShotIdentity.cpp
    rollback/EffectIdentity.cpp
    rollback/PotionIdentity.cpp
    level/WorldMotion.cpp
    party/TurboEmission.cpp
    party/TurboEffectResources.cpp
    party/MeleeGeometry.cpp
    netplay/MatchSession.cpp
    netplay/ShopSync.cpp
    netplay/OnlineSession.cpp
    characters/CharacterProfile.cpp
    rollback/PartyCheckpoint.cpp
    netplay/PartyBootstrap.cpp
    online/NativeRollbackDriver.cpp
    online/NativePlaySetup.cpp
    online/NativeOnlinePlay.cpp
    online/PartyStore.h
    online/OnlineLinks.h
    netplay/OnlineParty.cpp
    online/OnlineLevelEntry.cpp
    online/OnlineCompletion.cpp
    app/OnlineRun.cpp
    app/NativeOnlineIdentity.cpp
    app/NumericWitness.cpp
    scenes/PlaySceneSimulation.cpp
    party/PlayerNativeState.cpp
    party/PlayInput.h
    party/PlayInputReader.h
    party/PlayInputReader.cpp
    rollback/SessionInputs.cpp
    level/StaticScenery.cpp
    enemies/DeathRules.cpp
    enemies/EnemiesDeath.cpp
    enemies/LevelOpponentsDeath.cpp
    enemies/LevelOpponentsLoot.cpp
    level/SecretChallenge.cpp
    hud/ChallengeHud.cpp
    hud/RuneMeter.cpp
    scenes/PlaySceneChallenge.cpp
    level/DestructibleWalls.cpp
    combat/MissileTarget.h
    combat/MissileTarget.cpp
    menu/LiveSettings.h
    menu/SettingsMenu.cpp
    menu/MainMenuFactory.cpp
    menu/SettingsRows.cpp
    widgets/TextHighlight.cpp
    menu/ControlSettings.cpp
    hud/ControlPrompts.cpp
    menu/VideoSettings.cpp
    widgets/MenuFont.cpp
    hud/CompassHud.cpp
    menu/SaveMenu.cpp
    menu/PauseMenu.cpp
    shop/ShopPurchase.h
    shop/ShopPurchase.cpp
    characters/LevelResults.h
    characters/LevelResults.cpp
    shop/InventoryPanel.h
    shop/InventoryPanel.cpp
    hud/LinkIndicator.cpp
    shop/ShopSession.h
    shop/ShopSession.cpp
    scenes/AfterLevelScene.h
    scenes/AfterLevelScene.cpp
    hud/AutoSaveIcon.cpp
    scenes/AfterLevelPresentation.cpp
    scenes/FinalStatsPresentation.cpp
    shop/ShopLayout.cpp
    app/Gauntlet.h
    app/Gauntlet.cpp
    app/ImportedShop.cpp
    app/ImportedShop.h
    enemies/LevelOpponentsImported.cpp
    app/GauntletNetplay.cpp
    app/GameCursor.cpp
    app/BuildLabel.cpp
    app/LevelDebugOverlay.cpp
    console/ConsoleActionSink.h
    console/ConsoleCommand.h
    console/ConsoleCommand.cpp
    console/ConsoleForm.h
    console/ConsoleHost.h
    console/ConsoleListings.h
    console/ConsoleListings.cpp
    console/ConsolePolicy.h
    console/ConsolePolicy.cpp
    console/ConsoleReach.h
    console/ConsoleWords.h
    console/DevConsole.h
    console/DevConsole.cpp
    console/ConsoleArguments.h
    console/ConsoleArguments.cpp
    console/ConsoleUsage.h
    console/ConsoleUsage.cpp
    console/GiftRequest.h
    console/GiftRequest.cpp
    console/GotoRequest.h
    console/GotoRequest.cpp
    console/SpawnRequest.h
    console/SpawnRequest.cpp
    console/FixtureRequest.h
    console/FixtureRequest.cpp
    console/PartyCountRequest.h
    console/PartyCountRequest.cpp
    console/LevelListingLines.h
    console/LevelListingLines.cpp
    rollback/SharedConsoleWords.h
    rollback/SharedConsoleWords.cpp
    app/GauntletConsole.cpp
    app/ConsoleActionRoute.h
    app/ConsoleActionRoute.cpp
    app/ConsoleRoster.h
    app/ConsoleRoster.cpp
    app/OnlineConsoleCommands.h
    app/OnlineConsoleCommands.cpp
    app/ViewConsoleCommands.h
    app/ViewConsoleCommands.cpp
    app/InspectConsoleForm.h
    app/InspectConsoleForm.cpp
    app/PartyCountCommand.h
    app/PartyCountCommand.cpp
    app/ProfileConsoleCommands.h
    app/ProfileConsoleCommands.cpp
    app/LevelCompletion.h
    audio/MusicDuck.h
    audio/MusicDuck.cpp
    app/CommandLine.h
    app/CommandLine.cpp
    app/AssetRoots.h
    app/AssetRoots.cpp
    app/Scenario.h
    app/Scenario.cpp
    app/AttractSequencer.h
    app/AttractSequencer.cpp
    config/GameConfig.h
    config/MultiplayerMode.h
    config/GameConfig.cpp
    config/BindingNames.h
    config/BindingNames.cpp
    config/ControlProfiles.cpp
    characters/ClassData.h
    players/MagicPerks.h
    players/MagicPerks.cpp
    characters/ClassData.cpp
    characters/NativeClassData.h
    characters/NativeClassData.cpp
    characters/Progression.h
    characters/Progression.cpp
    characters/CharacterSave.h
    characters/CharacterSave.cpp
    characters/AutoSave.cpp
    characters/NameCheats.h
    characters/NameCheats.cpp
    players/TurboMeter.h
    players/TurboMeter.cpp
    players/TurboMove.cpp
    characters/Party.h
    characters/Party.cpp
    characters/Inventory.h
    characters/Inventory.cpp
    players/MikeyDecoy.cpp
    characters/Relics.h
    characters/Relics.cpp
    characters/LevelRecord.h
    characters/LevelRecord.cpp
    players/ItemPickup.h
    players/ItemPickup.cpp
    players/PickupVoices.cpp
    combat/Damage.h
    combat/BodyContact.h
    combat/BodyContact.cpp
    combat/Damage.cpp
    combat/DamageTypes.cpp
    combat/StrikeHit.h
    combat/StrikeHit.cpp
    combat/CombatArenaTarget.h
    combat/HurtKind.h
    combat/PowerupFlags.h
    party/PlayerPowerups.h
    party/PlayerPowerups.cpp
    players/PowerupEffects.h
    players/PowerupEffects.cpp
    players/EnemyShrink.h
    players/EnemyShrink.cpp
    players/PowerupEndings.h
    players/PowerupEndings.cpp
    players/PlayerControls.h
    players/PlayerControls.cpp
    players/CursorAim.cpp
    console/NoclipMotion.cpp
    players/PlayerActor.h
    players/PlayerActor.cpp
    players/PlayerCapture.h
    players/PlayerCapture.cpp
    players/ComboMove.h
    players/ComboMove.cpp
    party/PartyCombo.h
    party/PartyCombo.cpp
    players/PlayerTransport.h
    players/PlayerTransport.cpp
    players/PlayerDeeds.h
    players/PlayerAnimator.h
    players/PlayerAnimator.cpp
    players/MeleeSelection.cpp
    players/Knockback.h
    players/Knockback.cpp
    players/PlayerImpact.h
    players/PlayerImpact.cpp
    level/BossKinds.h
    level/BossKinds.cpp
    level/LevelCatalog.h
    level/LevelCatalog.cpp
    level/LevelItemArchives.h
    level/LevelItemArchives.cpp
    level/ItemFigure.h
    level/ItemFigure.cpp
    level/MissileStop.h
    level/CombatantObstacle.h
    level/FixtureHost.h
    level/CombatCover.h
    party/AttackHost.h
    party/AttackTargetId.h
    enemies/OpponentHost.h
    enemies/BossEncounterHost.h
    enemies/OpponentPool.h
    enemies/OpponentCues.h
    enemies/EncounterLevel.h
    level/ItemSupport.cpp
    party/BodyCollision.cpp
    party/MoveStrikes.h
    party/MoveStrikes.cpp
    level/Breakables.h
    level/Breakables.cpp
    level/Rubble.h
    level/Rubble.cpp
    level/SafeRocks.h
    level/SafeRocks.cpp
    level/SafeRockCover.h
    level/SafeRockCover.cpp
    level/ImportedCover.h
    level/ImportedCover.cpp
    characters/LevelWatch.cpp
    enemies/EnemyKinds.cpp
    enemies/EnemyAnimator.cpp
    enemies/EnemyFeedback.h
    enemies/EnemyFeedback.cpp
    enemies/EnemyMind.cpp
    enemies/EnemyMissiles.cpp
    enemies/Enemies.cpp
    enemies/Generators.cpp
    critters/CritterData.cpp
    critters/NativeCritterData.cpp
    critters/CritterMovement.cpp
    critters/CritterPatrol.cpp
    critters/CritterStatues.cpp
    enemies/PlacedEnemies.cpp
    critters/CombatantBreath.cpp
    critters/CombatantProjectile.cpp
    critters/CombatantProjectiles.cpp
    critters/Critters.cpp
    critters/Combatant.cpp
    critters/CombatantParts.cpp
    critters/CombatantNodes.cpp
    critters/CombatantGaze.cpp
    critters/MoveDefinition.cpp
    critters/CombatantAssets.cpp
    critters/CombatantMoves.cpp
    critters/CombatantMotion.cpp
    critters/CombatantAttacks.cpp
    critters/CombatantGrabs.cpp
    critters/CombatantView.cpp
    critters/CombatantVisual.cpp
    critters/Golem.cpp
    critters/General.cpp
    critters/Gargoyle.cpp
    bosses/BossDefinition.cpp
    bosses/BossTraits.h
    bosses/BossTraits.cpp
    critters/CombatantPatterns.cpp
    critters/CritterArea.cpp
    critters/CritterPressure.cpp
    critters/CombatantAreas.cpp
    bosses/BossCoins.cpp
    bosses/SkorneRelics.cpp
    bosses/Bosses.cpp
    bosses/LegendItems.cpp
    level/Chests.h
    level/Chests.cpp
    level/LockedGates.h
    level/LockedGates.cpp
    level/Traps.h
    level/Traps.cpp
    level/TowerAccess.h
    level/TowerAccess.cpp
    level/InstanceNames.h
    level/InstanceNames.cpp
    level/ExitPortals.h
    level/ExitPortals.cpp
    level/LevelTransporters.h
    level/LevelTransporters.cpp
    level/LevelWorld.h
    level/LevelWorld.cpp
    level/WorldLightning.cpp
    level/WorldDestruction.cpp
    camera/TowerCamera.h
    camera/TowerCamera.cpp
    camera/BossCamera.h
    camera/BossCamera.cpp
    camera/CameraShake.h
    camera/CameraShake.cpp
    camera/StartCamera.h
    camera/StartCamera.cpp
    camera/FreeCamera.h
    camera/FreeCamera.cpp
    tower/SumnerFigure.h
    tower/SumnerFigure.cpp
    tower/SumnerHints.h
    tower/SumnerHints.cpp
    party/PlayerMissiles.h
    party/PlayerMissiles.cpp
    effects/MissileStreak.cpp
    party/TargetAssist.cpp
    effects/BlobShadow.cpp
    party/PlayerFigure.cpp
    party/PlayerFamiliar.cpp
    party/MikeyFigure.cpp
    effects/WeaponGlow.cpp
    camera/CameraMovementLimit.cpp
    level/SkorneArena.cpp
    level/FallingPiece.cpp
    level/FallingScenery.cpp
    tower/TowerPromotion.cpp
    tower/TowerRelics.cpp
    tower/TowerCompletion.cpp
    scenes/TowerCeremonies.cpp
    scenes/PlaySceneCeremonies.cpp
    scenes/PlaySceneRewind.cpp
    party/PlayerArsenal.cpp
    party/PotionEmission.cpp
    audio/LevelSoundscape.cpp
    audio/VoiceQueue.cpp
    effects/EffectTrees.h
    effects/ThermalTextures.h
    effects/DynamicLights.cpp
    effects/EffectTrees.cpp
    level/PlacedItems.h
    level/PlacedItems.cpp
    level/SpawnedObjects.h
    level/SpawnedObjects.cpp
    audio/AmbientSounds.h
    audio/AmbientSounds.cpp
    audio/MusicAreas.h
    audio/MusicAreas.cpp
    level/LevelTriggers.h
    level/LevelTriggers.cpp
    level/HazardSurfaces.h
    level/HazardSurfaces.cpp
    level/Rotators.h
    level/Rotators.cpp
    widgets/ScrollBox.h
    widgets/DialoguePresentation.h
    widgets/ScrollBox.cpp
    widgets/MenuInput.h
    widgets/MenuInput.cpp
    widgets/OptionMenu.h
    widgets/OptionMenu.cpp
    widgets/PointerTargets.h
    widgets/PointerTargets.cpp
    widgets/SceneInput.h
    widgets/SceneInput.cpp
    tower/HintMenu.h
    tower/HintMenu.cpp
    widgets/BurnDialogueScroll.h
    widgets/BurnDialogueScroll.cpp
    menu/NameEntry.h
    menu/NameEntry.cpp
    context/GameServices.h
    context/GameServices.cpp
    scenes/TitleScene.h
    scenes/TitleScene.cpp
    scenes/MovieScene.h
    scenes/MovieScene.cpp
    scenes/IdleScreen.cpp
    scenes/IdleScreen.h
    scenes/AttractScene.cpp
    scenes/AttractScene.h
    scenes/SmokeTestScene.h
    scenes/SmokeTestScene.cpp
    scenes/SelectLane.h
    scenes/SelectLane.cpp
    hud/StatusBox.h
    hud/StatusBox.cpp
    hud/CompanionArrows.h
    hud/CompanionArrows.cpp
    mechanics/BirthRules.h
    mechanics/BlowRules.h
    mechanics/FallRules.h
    mechanics/GameplayMechanic.h
    mechanics/MechanicCatalog.h
    mechanics/MechanicCatalog.cpp
    mechanics/MechanicsFacade.h
    mechanics/MechanicsFacade.cpp
    mechanics/Overlays.h
    mechanics/RunRules.h
    mechanics/SwarmAppearance.h
    mechanics/SwarmRules.h
    hud/PowerupSelector.h
    hud/PowerupSelector.cpp
    hud/HelpMessages.h
    hud/HelpMessages.cpp
    scenes/TransitionScreen.h
    scenes/TransitionScreen.cpp
    scenes/LevelLoadingScreen.h
    scenes/LevelLoadingScreen.cpp
    scenes/MapPicture.h
    scenes/MapPicture.cpp
    party/PortalDeparture.h
    party/PortalDeparture.cpp
    tower/LevelExitSpeech.cpp
    hud/PickupHud.h
    hud/PickupHud.cpp
    hud/BossMeter.h
    hud/BossMeter.cpp
    hud/HealthMeter.h
    bosses/BossVictory.h
    bosses/BossVictory.cpp
    bosses/BossVictoryPresentation.cpp
    bosses/BossSequence.cpp
    tower/LevelArrivalPresentation.cpp
    hud/LevelMessages.cpp
    tower/GameOver.cpp
    tower/SumnerVisit.cpp
    scenes/PlayerSelectScene.h
    scenes/PlayerSelectScene.cpp
    bosses/LegendPresentation.h
    audio/LegendSound.h
    bosses/LegendPresentation.cpp
    scenes/PlayScene.h
    scenes/Haptics.h
    scenes/ClassUnlocks.h
    scenes/ScenePicking.h
    scenes/SeatCameras.h
    scenes/TowerCeremonies.h
    party/PartyMotion.cpp
    party/PartyCollision.cpp
    party/FloorRiding.cpp
    party/PartyFigures.cpp
    party/PartyNames.cpp
    hud/PartyHud.cpp
    party/PartyRecords.cpp
    level/TriggerCues.cpp
    tower/TowerWelcome.cpp
    party/PickupPresentation.h
    party/PartyPickups.cpp
    level/LevelFixtures.cpp
    enemies/LevelOpponents.cpp
    enemies/ImportedOpponentCues.h
    enemies/ImportedOpponentCues.cpp
    enemies/OpponentSimulation.cpp
    enemies/OpponentPlacements.cpp
    party/PlayerAttacks.cpp
    enemies/EnemyMissileEmission.cpp
    effects/CritterCueEmission.cpp
    party/PartyLighting.cpp
    enemies/EnemyFeedbackEmission.cpp
    party/PartyDamage.cpp
    party/PlayerItemAttacks.cpp
    players/ItemAttack.h
    players/ItemAttack.cpp
    effects/HeadGem.cpp
    effects/BodyGlow.cpp
    players/MeleeStreak.cpp
    players/ExitWait.cpp
    party/PowerupCompanion.cpp
    effects/WeaponTrail.cpp
    party/PlayerHealth.cpp
    scenes/PlayScene.cpp
    scenes/PlaySceneImported.cpp
    scenes/PlaySceneConsole.cpp
    scenes/SharedConsoleVerbs.h
    scenes/SharedConsoleVerbs.cpp
    scenes/SharedConsoleCommand.h
    scenes/SharedConsoleCommand.cpp
    scenes/LevelListings.h
    scenes/LevelListings.cpp
    scenes/FixtureConsoleCommands.h
    scenes/FixtureConsoleCommands.cpp
    scenes/ConsoleReplies.h
    scenes/ConsoleReplies.cpp
    scenes/ConsoleStage.h
    scenes/ConsoleStage.cpp
    scenes/ConsoleTravel.h
    scenes/ConsoleTravel.cpp
    scenes/SceneDescription.h
    scenes/SceneDescription.cpp
    scenes/PlaySceneTransport.cpp
    scenes/PlaySceneMotion.cpp
    scenes/PlaySceneHarm.cpp
    scenes/PlaySceneOpponents.cpp
    scenes/PlaySceneCamera.cpp
    scenes/ScenePicking.cpp
    scenes/SeatCameras.cpp
    scenes/SceneSelection.cpp
    scenes/PlaySceneSwitch.cpp
    level/SwitchCutscene.cpp
    level/SwitchCutscene.h)

add_library(gdl::game ALIAS game)
gdl_add_expansion_game_sources(game)

# The online compatibility identity digests the sources rather than the executable. The
# digest is taken on every build; the unit is rewritten only when it changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GDL_SOURCE_IDENTITY_UNIT "${CMAKE_BINARY_DIR}/generated/game/app/SourceIdentity.cpp")
add_custom_target(source_identity
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/checks/source_identity.py"
        --root "${CMAKE_SOURCE_DIR}" --write "${GDL_SOURCE_IDENTITY_UNIT}"
    BYPRODUCTS "${GDL_SOURCE_IDENTITY_UNIT}"
    COMMENT "Digesting the game sources"
    VERBATIM)
target_sources(game PRIVATE "${GDL_SOURCE_IDENTITY_UNIT}" app/SourceIdentity.h)
add_dependencies(game source_identity)

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(game PUBLIC gdl::engine PRIVATE gdl::formats nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
if(GDL_ENABLE_NETPLAY)
    target_link_libraries(game PRIVATE gdl::engine_net)
endif()
set_source_files_properties(app/OnlineRun.cpp PROPERTIES
    COMPILE_DEFINITIONS GDL_ENABLE_NETPLAY=$<BOOL:${GDL_ENABLE_NETPLAY}>)
gdl_apply_compiler_options(game)
set_target_properties(game PROPERTIES FOLDER "game")

add_executable(gauntlet main.cpp)

target_link_libraries(gauntlet PRIVATE gdl::game gdl::mods)
gdl_use_utf8_code_page(gauntlet)

# On Windows the executable carries the memory-card icon from the disc, which gdlunpack
# converts while building; nothing from the assets is checked in.
get_filename_component(GDL_DISC_DIR "${GDL_ASSET_DIR}" DIRECTORY)
set(GDL_CARD_ICON "${GDL_DISC_DIR}/carddemo/icon.tpl")
if(WIN32 AND GDL_EMBED_DISC_ICON AND EXISTS "${GDL_CARD_ICON}")
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
    GDL_VERSION="${GDL_VERSION}"
    GDL_DEFAULT_ASSET_DIR="${GDL_ASSET_DIR}"
    GDL_DEFAULT_DATA_DIR="${GDL_DATA_DIR}")

gdl_apply_compiler_options(gauntlet)

set_target_properties(gauntlet PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "game")

gdl_add_shaders(gauntlet
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/shaders"
    SOURCES
        ${CMAKE_SOURCE_DIR}/shaders/immediate.vert
        ${CMAKE_SOURCE_DIR}/shaders/immediate.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess.vert
        ${CMAKE_SOURCE_DIR}/shaders/bloom.frag
        ${CMAKE_SOURCE_DIR}/shaders/bloom_ms.frag
        ${CMAKE_SOURCE_DIR}/shaders/ambient_occlusion.frag
        ${CMAKE_SOURCE_DIR}/shaders/ambient_occlusion_ms.frag
        ${CMAKE_SOURCE_DIR}/shaders/depth_of_field.frag
        ${CMAKE_SOURCE_DIR}/shaders/depth_of_field_ms.frag
    INCLUDES ${CMAKE_SOURCE_DIR}/shaders/platform.glsl
             ${CMAKE_SOURCE_DIR}/shaders/immediate_constants.glsl
             ${CMAKE_SOURCE_DIR}/shaders/depth_of_field.glsl
             ${CMAKE_SOURCE_DIR}/shaders/bloom.glsl
             ${CMAKE_SOURCE_DIR}/shaders/ambient_occlusion.glsl
             ${CMAKE_SOURCE_DIR}/shaders/scene_depth.glsl)
