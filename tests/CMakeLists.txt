find_package(Catch2 CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
include(Catch)
add_subdirectory(expansions/audio)

# Detailed state comparisons retain legacy capture/catalog codecs only in tests.
# No production target links these snapshots or their compression dependency.
find_package(lz4 CONFIG REQUIRED)
add_library(game_diagnostics STATIC
    game/critters/CombatantCheckpoint.cpp
    game/critters/CrittersCheckpoint.cpp
    game/enemies/PlacedEnemiesCheckpoint.cpp
    game/party/PlayerAttacksCheckpoint.cpp
    game/enemies/OpponentCombatCheckpoint.cpp
    game/level/LevelFixturesCheckpoint.cpp
    game/level/ItemFigureCheckpoint.cpp
    game/critters/CombatantProjectilesCheckpoint.cpp
    game/level/ChestsCheckpoint.cpp
    game/level/LockedGatesCheckpoint.cpp
    game/level/BreakablesCheckpoint.cpp
    game/level/SafeRocksCheckpoint.cpp
    game/level/StaticSceneryCheckpoint.cpp
    game/level/RubbleCheckpoint.cpp
    game/level/TrapsCheckpoint.cpp
    game/level/DestructibleWallsCheckpoint.cpp
    game/level/PlacedItemsCheckpoint.cpp
    game/level/LevelTransportersCheckpoint.cpp
    game/level/ExitPortalsCheckpoint.cpp
    engine/world/WorldStateCheckpoint.cpp
    game/party/PlayerMissilesCheckpoint.cpp
    game/effects/EffectTreesCheckpoint.cpp
    game/level/RotatorsCheckpoint.cpp
    game/level/LevelWorldCheckpoint.cpp
    game/party/PlayerArsenalCheckpoint.cpp
    game/level/WorldDestructionCheckpoint.cpp
    game/level/FallingSceneryCheckpoint.cpp
    game/camera/TowerCameraCheckpoint.cpp
    game/rollback/PlayerHealthCheckpoint.cpp
    game/rollback/PlayerPoseCheckpoint.cpp
    game/rollback/PlayerPotionCheckpoint.cpp
    game/rollback/TurboEffectCheckpoint.cpp
    game/rollback/PlayerActionCheckpoint.cpp
    game/rollback/AnimationCheckpoint.cpp
    game/rollback/RandomCheckpoint.cpp
    game/enemies/EnemyMissilesCheckpoint.cpp
    game/rollback/EnemyPoolCheckpoint.cpp
    game/rollback/GeneratorCheckpoint.cpp
    game/rollback/PlacedEnemyCheckpoint.cpp
    game/rollback/PlayerTurboCheckpoint.cpp
    game/rollback/PlayerKnockbackCheckpoint.cpp
    game/party/PlayerRuntimeCheckpoint.cpp
    game/enemies/OpponentState.cpp
    game/enemies/OpponentResources.cpp
    game/enemies/OpponentCheckpoint.cpp
    game/enemies/CombatResources.cpp
    game/enemies/CombatRollbackCheckpoint.cpp
    game/scenes/StageResources.cpp
    game/rollback/CombatStatePacket.cpp
    game/rollback/StageStatePacket.cpp
    game/rollback/PlayerRuntimePacket.cpp)
target_include_directories(game_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_diagnostics PUBLIC gdl::game PRIVATE lz4::lz4)
gdl_apply_compiler_options(game_diagnostics)

add_executable(tests
    expansions/n64/level/LegendsUnlocksTests.cpp
    expansions/n64/level/LegendsPortalAppearanceTests.cpp
    expansions/n64/bosses/LegendsBossesTests.cpp
    expansions/n64/bosses/LegendsBossIntegrationTests.cpp
    expansions/n64/bosses/LegendsBossAssetsTests.cpp
    expansions/n64/bosses/LegendsPlagueTests.cpp
    expansions/n64/formats/LegendsPlagueMaterialTests.cpp
    expansions/n64/bosses/LegendsLegendTests.cpp
    expansions/n64/enemies/LegendsDeathTests.cpp
    expansions/n64/scenes/LegendsDeathIntegrationTests.cpp
    expansions/n64/bosses/LegendsBossMeleeTests.cpp
    expansions/n64/bosses/LegendsBossBreathTests.cpp
    expansions/n64/bosses/LegendsSkorneGameplayTests.cpp
    expansions/n64/level/LegendsBossEruptionTests.cpp
    expansions/n64/formats/LegendsN64CritterTests.cpp
    expansions/n64/critters/LegendsCrittersTests.cpp
    expansions/shared/enemies/LegendsActorsTests.cpp
    expansions/n64/enemies/LevelOpponentsLegendsTests.cpp
    expansions/n64/scenes/LegendsLevelProgressTests.cpp
    expansions/n64/scenes/LegendsSceneArenaTests.cpp
    expansions/n64/scenes/LegendsArenaCombatTests.cpp
    expansions/n64/scenes/LegendsBossRewardsTests.cpp
    expansions/n64/scenes/LegendsBossAudioTests.cpp
    engine/io/NativeStateDigestTests.cpp
    game/party/PlayerNativeStateTests.cpp
    game/enemies/EnemyMissilesNativeDigestTests.cpp
    game/critters/CombatantProjectilesNativeDigestTests.cpp
    game/level/LevelTriggersNativeDigestTests.cpp
    game/level/SecretChallengeNativeDigestTests.cpp
    game/level/NativeStateResourcesTests.cpp
    game/enemies/GeneratorNativeDigestTests.cpp
    game/enemies/EnemiesNativeDigestTests.cpp
    game/critters/CombatantNativeDigestTests.cpp
    game/enemies/PlacedEnemiesNativeDigestTests.cpp
    game/enemies/OpponentOwnersNativeDigestTests.cpp
    game/critters/CritterStatuesNativeDigestTests.cpp
    game/camera/CameraMeterNativeDigestTests.cpp
    game/effects/EffectTreesNativeDigestTests.cpp
    game/party/PlayerAttackNativeDigestTests.cpp
    game/party/PlayerFigureNativeDigestTests.cpp
    game/level/LevelWorldNativeDigestTests.cpp
    game/level/LevelFixturesNativeDigestTests.cpp
    game/audio/SceneAudioNativeDigestTests.cpp
    game/scenes/SceneTravelNativeDigestTests.cpp
    game/hud/SceneHudNativeDigestTests.cpp
    game/scenes/PlaySceneNativeStateDigestTests.cpp
    game/rollback/LocalStateHistoryTests.cpp
    game/netplay/RollbackExchangeTests.cpp
    game/audio/NativeSoundIdentityTests.cpp
    game/rollback/RollbackInputsTests.cpp
    game/rollback/InputJournalTests.cpp
    game/rollback/FrameEventLedgerTests.cpp
    game/party/PlayerLocalRewindTests.cpp
    game/party/PlayerSimulationEqualityTests.cpp
    game/rollback/NativeProbeTimingTests.cpp
    engine/app/ApplicationTimingTests.cpp
    engine/math/Mt19937Tests.cpp
    game/netplay/HostedRoomTests.cpp
    game/rollback/InputCommandTests.cpp
    game/rollback/InputTimelineTests.cpp
    game/rollback/ShotIdentityTests.cpp
    game/rollback/PlayerActionCheckpointTests.cpp
    game/rollback/PlayerTurboCheckpointTests.cpp
    game/rollback/AnimationCheckpointTests.cpp
    game/rollback/RandomCheckpointTests.cpp
    game/enemies/EnemyMissilesPacketTests.cpp
    game/rollback/EnemyPoolCheckpointTests.cpp
    game/rollback/GeneratorCheckpointTests.cpp
    game/rollback/PlacedEnemyCheckpointTests.cpp
    game/enemies/OpponentPlacementsTests.cpp
    game/party/PlayerAttacksCheckpointTests.cpp
    game/party/PlayerRuntimeCheckpointTests.cpp
    game/enemies/CombatRollbackCheckpointTests.cpp
    game/rollback/PlayerRuntimePacketTests.cpp
    game/rollback/CombatStatePacketTests.cpp
    game/rollback/PlayerHealthCheckpointTests.cpp
    game/enemies/OpponentCombatCheckpointTests.cpp
    game/enemies/OpponentLootCheckpointTests.cpp
    game/rollback/StageStatePacketTests.cpp
    game/rollback/CanonicalDigestSinkTests.cpp
    game/party/PlayerCompanionCheckpointTests.cpp
    game/party/PartyLightingTests.cpp
    game/level/ExitPortalsCheckpointTests.cpp
    game/level/LevelTransportersCheckpointTests.cpp
    game/enemies/CombatResourcesTests.cpp
    game/level/ChestsCheckpointTests.cpp
    game/level/ChestsTests.cpp
    game/level/LockedGatesCheckpointTests.cpp
    game/level/BreakablesCheckpointTests.cpp
    game/level/SafeRocksCheckpointTests.cpp
    game/level/StaticSceneryCheckpointTests.cpp
    game/level/RubbleCheckpointTests.cpp
    game/level/TrapsCheckpointTests.cpp
    game/level/FallingSceneryCheckpointTests.cpp
    game/level/DestructibleWallsCheckpointTests.cpp
    game/level/RotatorsCheckpointTests.cpp
    game/level/RotatorsLocalRewindTests.cpp
    game/level/PlacedItemsCheckpointTests.cpp
    game/level/PreparedPickupBirthTests.cpp
    game/level/LevelFixturesCheckpointTests.cpp
    game/level/LevelWorldCheckpointTests.cpp
    game/camera/TowerCameraCheckpointTests.cpp
    engine/world/WorldStateCheckpointTests.cpp
    engine/world/WorldCollapsedCollisionTests.cpp
    game/level/WorldDestructionCheckpointTests.cpp
    game/enemies/EnemyFeedbackEmissionTests.cpp
    game/party/PartyDamageTests.cpp
    game/party/PlayerArsenalCheckpointTests.cpp
    game/party/MoveStrikesCheckpointTests.cpp
    game/rollback/EffectIdentityTests.cpp
    game/rollback/PlayerPoseCheckpointTests.cpp
    game/rollback/PlayerPotionCheckpointTests.cpp
    game/level/WorldMotionTests.cpp
    game/party/TurboEmissionTests.cpp
    game/party/TurboEffectResourcesTests.cpp
    game/rollback/TurboEffectCheckpointTests.cpp
    game/party/MeleeGeometryTests.cpp
    game/rollback/PlayerKnockbackCheckpointTests.cpp
    game/players/MeleeSelectionTests.cpp
    game/players/PlayerActorCheckpointTests.cpp
    game/enemies/EnemyAnimatorCheckpointTests.cpp
    game/enemies/EnemiesCheckpointTests.cpp
    game/enemies/GeneratorsCheckpointTests.cpp
    game/enemies/EnemyMissilesCheckpointTests.cpp
    game/enemies/PlacedEnemiesTests.cpp
    game/critters/CritterStatuesCheckpointTests.cpp
    game/enemies/OpponentSimulationTests.cpp
    game/enemies/OpponentResourcesTests.cpp
    game/enemies/CombatProgressCheckpointTests.cpp
    game/netplay/MatchSessionTests.cpp
    game/netplay/ShopSyncTests.cpp
    game/online/OnlineCompletionTests.cpp
    game/online/OnlineLevelEntryTests.cpp
    game/netplay/OnlineSessionTests.cpp
    game/characters/CharacterProfileTests.cpp
    game/characters/PartyCheckpointTests.cpp
    game/netplay/PartyBootstrapTests.cpp
    game/netplay/OnlinePartyTests.cpp
    game/netplay/MeasuredPacketTransportTests.cpp
    game/rollback/SessionInputsTests.cpp
    game/app/BuildLabelTests.cpp
    game/app/LevelDebugOverlayTests.cpp
    game/console/DevConsoleTests.cpp
    game/console/ConsoleCommandsTests.cpp
    game/app/GameCursorTests.cpp
    game/scenes/PlaySceneRumbleTests.cpp
    game/level/StaticSceneryTests.cpp
    game/config/ControlProfilesTests.cpp
    game/hud/ControlPromptsTests.cpp
    game/players/CursorAimTests.cpp
    game/enemies/DeathRulesTests.cpp
    game/enemies/EnemiesDeathTests.cpp
    game/enemies/LevelOpponentsDeathTests.cpp
    game/scenes/PlaySceneAmbienceTests.cpp
    game/level/DestructibleWallsTests.cpp
    game/party/MissileTargetTests.cpp
    game/combat/ArmorFlagsTests.cpp
    game/combat/DamageTests.cpp
    game/combat/BodyContactTests.cpp
    game/combat/DamageTypesTests.cpp
    game/combat/EffectFlagsTests.cpp
    game/combat/PowerupFlagsTests.cpp
    game/party/PlayerPowerupsTests.cpp
    game/menu/SettingsMenuTests.cpp
    game/widgets/MenuFontTests.cpp
    game/menu/SettingsRowsTests.cpp
    game/menu/VideoSettingsTests.cpp
    game/hud/CompassHudTests.cpp
    game/menu/SaveMenuTests.cpp
    game/menu/PauseMenuTests.cpp
    formats/ShopWadTests.cpp
    game/shop/ShopTests.cpp
    game/scenes/AfterLevelSceneTests.cpp
    game/hud/AutoSaveIconTests.cpp
    game/scenes/PostShopReturnTests.cpp
    game/shop/InventoryPanelTests.cpp
    game/hud/LinkIndicatorTests.cpp
    TestSupport.h
    fixtures/ReferenceObj.cpp
    fixtures/ReferenceObjTests.cpp
    fixtures/NativeModelFixture.cpp
    fixtures/ReferenceWav.cpp
    fixtures/ReferenceWavTests.cpp
    fixtures/NativeSoundBank.cpp
    FakeRenderDevice.h
    engine/core/TypesTests.cpp
    engine/core/LogTests.cpp
    engine/core/AssertTests.cpp
    engine/core/ClockTests.cpp
    engine/math/MathTests.cpp
    engine/math/PortableMathTests.cpp
    engine/io/ByteReaderTests.cpp
    engine/io/FileTests.cpp
    engine/io/AssetLocatorTests.cpp
    engine/platform/InputTests.cpp
    engine/platform/GamepadsTests.cpp
    engine/platform/GlfwWindowTests.cpp
    engine/platform/DisplayTimingTests.cpp
    engine/platform/PathsTests.cpp
    engine/render/RenderTypesTests.cpp
    engine/render/VulkanTextureTests.cpp
    engine/render/VulkanCoverageTests.cpp
    engine/render/DepthOfFieldTests.cpp
    engine/render/HeatDistortionTests.cpp
    engine/render/ImageTests.cpp
    engine/render/ImmediateBatchTests.cpp
    engine/render/ThreadedRenderDeviceTests.cpp
    engine/render/OpenGlFunctionsTests.cpp
    engine/render/OpenGlShaderSourceTests.cpp
    engine/render/DebugTexturesTests.cpp
    engine/codec/LzssTests.cpp
    engine/codec/AviReaderTests.cpp
    engine/codec/VqVideoDecoderTests.cpp
    engine/codec/DspAdpcmTests.cpp
    engine/codec/AdsAudioTests.cpp
    engine/codec/MoviePlaybackTests.cpp
    engine/audio/AudioStreamTests.cpp
    engine/audio/AudioMixerTests.cpp
    engine/audio/AudioDeviceTests.cpp
    engine/app/ApplicationTests.cpp
    engine/render/VulkanFrameAcquireTests.cpp
    engine/core/StringsTests.cpp
    engine/assets/PngImageTests.cpp
    engine/assets/TextureSetTests.cpp
    engine/assets/TextureBindingsTests.cpp
    engine/assets/AssetScopeTests.cpp
    engine/assets/ObjectMaterialTests.cpp
    engine/world/AssetAuditTests.cpp
    engine/assets/NativeDataAuditTests.cpp
    engine/assets/NativeAssetsTests.cpp
    engine/assets/NativeShopTests.cpp
    engine/assets/BitmapFontTests.cpp
    engine/assets/SoundSetTests.cpp
    engine/audio/SoundPlayerTests.cpp
    engine/audio/AdsStreamTests.cpp
    expansions/dreamcast/audio/DreamcastStreamTests.cpp
    expansions/n64/audio/LegendsOpponentSoundsTests.cpp
    engine/audio/MusicDeclickerTests.cpp
    engine/audio/StreamPlaylistTests.cpp
    engine/assets/ModelSetTests.cpp
    engine/assets/AnimationSetTests.cpp
    engine/ui/ModelSpriteTests.cpp
    engine/world/AmbientDimmerTests.cpp
    engine/world/WorldCameraTests.cpp
    engine/world/WorldSceneTests.cpp
    engine/world/TextureAnimatorTests.cpp
    engine/world/WorldAnimatorTests.cpp
    game/level/WorldDestructionTests.cpp
    engine/world/ParticleSystemTests.cpp
    engine/world/ParticleCheckpointTests.cpp
    engine/world/ParticleFieldTests.cpp
    engine/world/ParticleLocalRewindTests.cpp
    engine/world/TreeParticlesTests.cpp
    engine/world/WorldCollisionTests.cpp
    engine/world/TreeModelTests.cpp
    engine/world/AnimationPlayerTests.cpp
    engine/world/TreePoseTests.cpp
    engine/assets/WorldLayoutTests.cpp
    formats/WorldFileTests.cpp
    formats/WorldDataWadTests.cpp
    engine/assets/WorldDataTests.cpp
    engine/assets/MessageTableTests.cpp
    game/widgets/ScrollBoxTests.cpp
    game/tower/SumnerFigureTests.cpp
    game/tower/SumnerHintsTests.cpp
    game/party/PlayerMissilesTests.cpp
    game/party/PlayerMissilesCheckpointTests.cpp
    game/effects/EffectTreesPoolCheckpointTests.cpp
    game/effects/EffectTreesLocalRewindTests.cpp
    game/level/ItemFigureLocalRewindTests.cpp
    game/party/TargetAssistTests.cpp
    game/party/BodyCollisionTests.cpp
    game/effects/BlobShadowTests.cpp
    game/party/PlayerFigureTests.cpp
    game/level/AssetCensusTests.cpp
    game/party/PlayerFamiliarTests.cpp
    game/party/MikeyFigureTests.cpp
    game/effects/WeaponGlowTests.cpp
    game/camera/CameraMovementLimitTests.cpp
    game/camera/FreeCameraTests.cpp
    game/console/NoclipMotionTests.cpp
    game/scenes/SceneSelectionTests.cpp
    game/scenes/ScenePickingTests.cpp
    game/scenes/SeatCamerasTests.cpp
    game/scenes/TowerCeremoniesTests.cpp
    engine/world/SelectionHighlightTests.cpp
    game/level/SkorneArenaTests.cpp
    game/level/FallingPieceTests.cpp
    game/level/FallingSceneryTests.cpp
    game/bosses/SkorneTests.cpp
    game/bosses/SpiderQueenTests.cpp
    game/bosses/LichTests.cpp
    game/party/PlayerArsenalTests.cpp
    game/party/PotionEmissionTests.cpp
    game/party/PotionFlightTests.cpp
    game/party/PojoTests.cpp
    game/party/PojoVoiceTests.cpp
    game/audio/LevelSoundscapeTests.cpp
    game/audio/VoiceQueueTests.cpp
    game/tower/HintMenuTests.cpp
    game/level/PlacedItemsTests.cpp
    game/level/SpawnedObjectsTests.cpp
    game/audio/AmbientSoundsTests.cpp
    game/audio/MusicAreasTests.cpp
    engine/world/WorldLightingTests.cpp
    engine/ui/CanvasTests.cpp
    engine/ui/TextPainterTests.cpp
    formats/ModelArchiveTests.cpp
    expansions/n64/formats/LegendsMapTests.cpp
    expansions/dreamcast/formats/LegendsDreamcastMapTests.cpp
    expansions/dreamcast/formats/LegendsDreamcastTextureTests.cpp
    expansions/n64/formats/LegendsN64TextureTests.cpp
    expansions/shared/scenes/LegendsPreviewTests.cpp
    expansions/n64/scenes/LegendsPlayableTests.cpp
    expansions/shared/scenes/LegendsTraversalTests.cpp
    expansions/dreamcast/scenes/LegendsMusicTests.cpp
    expansions/shared/scenes/LegendsMapCatalogTests.cpp
    expansions/shared/scenes/LegendsSceneAdapterTests.cpp
    expansions/shared/scenes/LegendsFixtureSelectionTests.cpp
    expansions/n64/scenes/LegendsSceneTrapsTests.cpp
    expansions/n64/scenes/LegendsLevelEffectsTests.cpp
    expansions/dreamcast/scenes/LegendsInitialPoseTests.cpp
    game/level/ItemFigureTextureLendersTests.cpp
    engine/assets/DecodedAssetsTests.cpp
    expansions/dreamcast/formats/LegendsDreamcastSceneTests.cpp
    expansions/n64/formats/LegendsN64SceneTests.cpp
    expansions/n64/formats/LegendsN64CameraTests.cpp
    expansions/n64/camera/LegendsCameraTests.cpp
    expansions/n64/level/LegendsObeliskTests.cpp
    expansions/n64/formats/LegendsN64AmbientTests.cpp
    expansions/n64/audio/LegendsAmbientTests.cpp
    expansions/n64/scenes/LegendsAmbientPlaybackTests.cpp
    expansions/n64/scenes/LegendsEntryTests.cpp
    expansions/n64/formats/LegendsN64TowerTests.cpp
    expansions/n64/formats/LegendsN64ProgressTests.cpp
    expansions/n64/formats/LegendsN64GatesTests.cpp
    expansions/n64/formats/LegendsN64TrapsTests.cpp
    expansions/n64/level/LegendsTowerProgressTests.cpp
    expansions/n64/level/LegendsTowerPortalsTests.cpp
    expansions/n64/level/LegendsGatesTests.cpp
    expansions/n64/formats/LegendsN64PortalTests.cpp
    expansions/n64/level/LegendsPortalsTests.cpp
    expansions/n64/formats/LegendsN64MorphTests.cpp
    expansions/n64/level/LegendsTowerMotionTests.cpp
    expansions/n64/level/LegendsSumnerTests.cpp
    expansions/n64/scenes/LegendsSumnerTextTests.cpp
    expansions/n64/scenes/LegendsSumnerIntegrationTests.cpp
    game/level/ImportedLevelWorldTests.cpp
    game/scenes/ImportedPlaySceneTests.cpp
    expansions/n64/scenes/LegendsGameplayTests.cpp
    expansions/n64/scenes/LegendsTowerTests.cpp
    expansions/n64/scenes/LegendsRidersTests.cpp
    expansions/n64/scenes/LegendsBattleSwitchTests.cpp
    expansions/n64/scenes/LegendsProgressionTests.cpp
    expansions/n64/bosses/LegendsChimeraTests.cpp
    expansions/n64/bosses/LegendsChimeraTargetingTests.cpp
    expansions/n64/menu/LegendsDialogueTests.cpp
    expansions/n64/scenes/LegendsWorldPresentationTests.cpp
    expansions/dreamcast/formats/LegendsSceneMotionTests.cpp
    expansions/n64/scenes/LegendsN64MusicTests.cpp
    expansions/shared/characters/LegendsProgressTests.cpp
    expansions/shared/characters/LegendsVisitTests.cpp
    expansions/shared/level/LegendsItemsTests.cpp
    expansions/n64/level/LegendsN64ItemsTests.cpp
    expansions/n64/level/LegendsContainersTests.cpp
    expansions/n64/level/LegendsBlastTests.cpp
    expansions/n64/level/LegendsTrapsTests.cpp
    expansions/n64/level/LegendsSceneryTests.cpp
    expansions/n64/scenes/LegendsSceneSceneryTests.cpp
    expansions/n64/enemies/LegendsGeneratorsTests.cpp
    expansions/n64/enemies/LegendsEnemyAssetsTests.cpp
    expansions/n64/enemies/LegendsEnemySoundsTests.cpp
    game/party/ImportedTargetsTests.cpp
    expansions/n64/formats/LegendsN64TextTests.cpp
    expansions/n64/formats/LegendsN64SwitchesTests.cpp
    expansions/n64/level/LegendsSwitchesTests.cpp
    expansions/n64/level/LegendsLootSelectorsTests.cpp
    expansions/n64/level/LegendsWorldMotionTests.cpp
    expansions/dreamcast/level/LegendsDreamcastItemsTests.cpp
    formats/GcTextureTests.cpp
    formats/FontFileTests.cpp
    formats/TextRomTests.cpp
    formats/JsonWriterTests.cpp
    formats/AudioRomTests.cpp
    formats/SoundBankTests.cpp
    formats/WavWriterTests.cpp
    formats/GeometryStreamTests.cpp
    formats/AnimationTreeTests.cpp
    formats/ObjWriterTests.cpp
    game/app/CommandLineTests.cpp
    game/app/ScenarioTests.cpp
    game/app/AttractSequencerTests.cpp
    game/app/LevelCompletionTests.cpp
    game/audio/MusicDuckTests.cpp
    game/scenes/MovieSceneTests.cpp
    game/widgets/MenuInputTests.cpp
    game/menu/MainMenuFactoryTests.cpp
    game/widgets/OptionMenuTests.cpp
    game/scenes/TitleSceneTests.cpp
    game/scenes/IdleScreenTests.cpp
    game/scenes/AttractSceneTests.cpp
    game/config/GameConfigTests.cpp
    game/characters/ClassDataTests.cpp
    game/characters/NativeClassDataTests.cpp
    game/critters/NativeCritterDataTests.cpp
    game/critters/CombatantBindingsTests.cpp
    game/app/AssetRootsTests.cpp
    game/scenes/NativeGameplayTests.cpp
    game/players/MagicPerksTests.cpp
    game/characters/ProgressionTests.cpp
    game/characters/CharacterSaveTests.cpp
    game/characters/AutoSaveTests.cpp
    game/characters/NameCheatsTests.cpp
    game/characters/PartyTests.cpp
    game/players/TurboMeterTests.cpp
    game/players/TurboMoveTests.cpp
    game/characters/InventoryTests.cpp
    game/players/MikeyDecoyTests.cpp
    game/characters/RelicsTests.cpp
    game/players/ItemPickupTests.cpp
    game/players/PowerupEffectsTests.cpp
    game/players/EnemyShrinkTests.cpp
    game/players/PowerupEndingsTests.cpp
    game/hud/PowerupSelectorTests.cpp
    game/scenes/TransitionScreenTests.cpp
    game/scenes/LevelLoadingScreenTests.cpp
    game/scenes/MapPictureTests.cpp
    game/party/PortalDepartureTests.cpp
    game/tower/LevelExitSpeechTests.cpp
    game/effects/DynamicLightsTests.cpp
    game/effects/EffectTreesTests.cpp
    game/effects/EffectCheckpointTests.cpp
    game/critters/CombatantProjectilesTests.cpp
    game/critters/CombatantProjectilesCheckpointTests.cpp
    game/players/PlayerControlsTests.cpp
    game/players/PlayerActorTests.cpp
    game/players/PlayerCaptureTests.cpp
    game/players/ComboMoveTests.cpp
    game/party/PartyComboTests.cpp
    game/players/PlayerTransportTests.cpp
    game/players/PlayerAnimatorTests.cpp
    game/players/PlayerAnimatorCadenceTests.cpp
    game/players/PlayerAnimatorInterruptTests.cpp
    game/party/ItemAttackTests.cpp
    game/players/KnockbackTests.cpp
    game/players/PlayerImpactTests.cpp
    game/level/LevelWorldTests.cpp
    game/level/LevelCatalogTests.cpp
    game/level/LevelItemArchivesTests.cpp
    game/level/TowerAccessTests.cpp
    game/characters/LevelRecordTests.cpp
    game/level/ExitPortalsTests.cpp
    game/level/LevelTransportersTests.cpp
    game/scenes/PlaySceneTransportTests.cpp
    game/scenes/PlaySceneSwitchTests.cpp
    game/level/SwitchCutsceneTests.cpp
    game/level/LevelFixturesFloorsTests.cpp
    game/level/RubbleTests.cpp
    game/level/SafeRocksTests.cpp
    game/characters/LevelWatchTests.cpp
    game/enemies/EnemyKindsTests.cpp
    game/enemies/EnemyAnimatorTests.cpp
    game/enemies/EnemyFeedbackTests.cpp
    game/enemies/EnemyMindTests.cpp
    game/enemies/EnemyMissilesTests.cpp
    game/enemies/EnemiesTests.cpp
    game/enemies/GeneratorsTests.cpp
    game/critters/CrittersTests.cpp
    game/critters/CombatantTests.cpp
    game/critters/CombatantCheckpointTests.cpp
    game/critters/CombatantLocalRewindTests.cpp
    game/critters/CrittersLocalRewindTests.cpp
    game/bosses/BossLocalRewindTests.cpp
    game/critters/CrittersCheckpointTests.cpp
    game/bosses/ChimeraTests.cpp
    game/critters/CombatantGazeTests.cpp
    game/critters/CombatantGrabsTests.cpp
    game/critters/CombatantNodesTests.cpp
    game/camera/CameraShakeTests.cpp
    game/critters/CritterAttackSelectionTests.cpp
    game/critters/CritterAreaTests.cpp
    game/critters/CritterPressureTests.cpp
    game/critters/CritterMovementTests.cpp
    game/critters/CritterPatrolTests.cpp
    game/critters/CritterStatuesTests.cpp
    game/critters/GargoyleTests.cpp
    game/critters/CombatantBreathTests.cpp
    game/critters/CombatantProjectileTests.cpp
    formats/CritterWadTests.cpp
    game/bosses/BossCoinsTests.cpp
    game/bosses/BossesTests.cpp
    game/bosses/LegendItemsTests.cpp
    game/party/MoveStrikesTests.cpp
    game/hud/HelpMessagesTests.cpp
    game/level/LevelTriggersTests.cpp
    game/level/LevelTriggersCheckpointTests.cpp
    game/level/HazardSurfacesTests.cpp
    game/level/RotatorsTests.cpp
    game/camera/TowerCameraTests.cpp
    game/camera/BossCameraTests.cpp
    game/camera/StartCameraTests.cpp
    game/hud/StatusBoxTests.cpp
    game/hud/CompanionArrowsTests.cpp
    game/mechanics/MechanicsFacadeTests.cpp
    game/hud/PickupHudTests.cpp
    game/hud/BossMeterTests.cpp
    game/hud/ChallengeHudTests.cpp
    game/hud/RuneMeterTests.cpp
    game/scenes/PlaySceneChallengeTests.cpp
    game/level/SecretChallengeTests.cpp
    game/bosses/BossVictoryTests.cpp
    game/bosses/BossVictoryPresentationTests.cpp
    game/bosses/BossSequenceTests.cpp
    game/tower/LevelArrivalPresentationTests.cpp
    game/hud/LevelMessagesTests.cpp
    game/tower/GameOverTests.cpp
    game/tower/SumnerVisitTests.cpp
    game/tower/TowerPromotionTests.cpp
    game/tower/TowerRelicsTests.cpp
    game/tower/TowerCompletionTests.cpp
    game/bosses/LegendPresentationTests.cpp
    game/scenes/PlaySceneTests.cpp
    game/scenes/PlaySceneG4Tests.cpp
    game/scenes/MultiplayerJourneyTests.cpp
    game/party/PartyMotionTests.cpp
    game/party/PartyCollisionTests.cpp
    game/effects/HeadGemTests.cpp
    game/effects/BodyGlowTests.cpp
    game/players/MeleeStreakTests.cpp
    game/players/ExitWaitTests.cpp
    game/party/PowerupCompanionTests.cpp
    game/effects/WeaponTrailTests.cpp
    game/party/FloorRidingTests.cpp
    game/level/NativeTraversalTests.cpp
    game/party/PartyFiguresTests.cpp
    game/party/PartyNamesTests.cpp
    game/hud/PartyHudTests.cpp
    game/party/PartyRecordsTests.cpp
    game/level/TriggerCuesTests.cpp
    game/tower/TowerWelcomeTests.cpp
    game/party/PartyPickupsTests.cpp
    game/level/LevelFixturesTests.cpp
    game/enemies/LevelOpponentsTests.cpp
    game/party/PlayerHealthTests.cpp
    game/party/PlayerAttacksTests.cpp
    game/menu/NameEntryTests.cpp
    game/scenes/SelectLaneTests.cpp
    game/scenes/PlayerSelectSceneTests.cpp
    formats/PlayerDataWadTests.cpp
    formats/TplFileTests.cpp
    formats/IcoWriterTests.cpp
    game/widgets/BurnDialogueScrollTests.cpp
    engine/assets/StringTableTests.cpp)

target_link_libraries(tests PRIVATE game_diagnostics gdl::engine gdl::formats gdl::game Catch2::Catch2WithMain SDL3::SDL3)
# Each mod's tests sit in tests/mods/<folder> and join the suite with the mod itself.
file(GLOB GDL_MOD_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mods/*/*.cpp")
target_sources(tests PRIVATE ${GDL_MOD_TESTS})
target_link_libraries(tests PRIVATE gdl::mods)
# Focused native gameplay/presentation checks without unrelated GPU or full-suite builds.
add_executable(gameplayregressions
    game/enemies/EnemiesTests.cpp game/critters/CombatantTests.cpp engine/world/TreeModelTests.cpp
    game/scenes/PostShopReturnTests.cpp
    game/level/LevelFixturesTests.cpp
    game/party/PowerupCompanionTests.cpp game/config/GameConfigTests.cpp
    game/shop/ShopTests.cpp game/characters/AutoSaveTests.cpp
    game/level/SecretChallengeTests.cpp game/scenes/PlaySceneChallengeTests.cpp
    game/audio/LevelSoundscapeTests.cpp game/scenes/PlaySceneAmbienceTests.cpp
    game/scenes/PlaySceneSwitchTests.cpp game/level/TriggerCuesTests.cpp
    engine/world/WorldAnimatorTests.cpp
    fixtures/NativeModelFixture.cpp fixtures/ReferenceObj.cpp fixtures/NativeSoundBank.cpp)
target_link_libraries(gameplayregressions PRIVATE game_diagnostics gdl::game Catch2::Catch2WithMain)
target_include_directories(gameplayregressions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gameplayregressions PRIVATE
    GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}" GDL_TEST_UNPACKED_DIR="${GDL_UNPACKED_DIR}"
    GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
gdl_apply_compiler_options(gameplayregressions)
set_target_properties(gameplayregressions PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
# Focused character-form, inventory and progression regressions with native gameplay checks.
add_executable(playerformtests game/party/PojoTests.cpp game/party/PojoVoiceTests.cpp game/characters/NameCheatsTests.cpp
    game/characters/InventoryTests.cpp game/characters/CharacterSaveTests.cpp
    game/party/PlayerFigureTests.cpp game/party/PlayerArsenalTests.cpp
    game/party/PlayerMissilesTests.cpp
    game/effects/EffectTreesTests.cpp
    game/party/PlayerAttacksTests.cpp
    game/party/PartyPickupsTests.cpp game/scenes/PlaySceneTests.cpp
    game/characters/CharacterProfileTests.cpp
    game/party/PowerupCompanionTests.cpp fixtures/NativeModelFixture.cpp
    fixtures/ReferenceObj.cpp fixtures/NativeSoundBank.cpp)
target_link_libraries(playerformtests PRIVATE gdl::game Catch2::Catch2WithMain)
target_include_directories(playerformtests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(playerformtests PRIVATE
    GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}" GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
gdl_apply_compiler_options(playerformtests)
set_target_properties(playerformtests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
if(GDL_ENABLE_NETPLAY)
    target_link_libraries(tests PRIVATE gdl::engine_net)
    target_sources(tests PRIVATE engine/net/SessionTransportTests.cpp game/app/OnlineRunTests.cpp
        game/netplay/NativeCampaignTransportTests.cpp)
    # Fast, asset-free native adapter and room checks, separate from GPU fixtures.
    add_executable(netplaytests engine/net/SessionTransportTests.cpp
        engine/io/Sha256Tests.cpp game/netplay/AssetDigestTests.cpp game/online/NetplayMenuTests.cpp
        game/netplay/HostedRoomTests.cpp game/netplay/OnlineSessionTests.cpp
        game/app/CommandLineTests.cpp)
    target_link_libraries(netplaytests PRIVATE gdl::game gdl::engine_net Catch2::Catch2WithMain)
    target_include_directories(netplaytests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gdl_apply_compiler_options(netplaytests)
    set_target_properties(netplaytests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
    # Focused native gameplay regressions without rebuilding unrelated test suites.
    add_executable(netplayplaytests game/netplay/OnlinePartyTests.cpp
        engine/assets/AssetScopeTests.cpp
        engine/world/WorldSceneTests.cpp
        engine/world/TreeModelTests.cpp
        game/rollback/ShotIdentityTests.cpp
        game/party/PlayerArsenalTests.cpp
        game/party/PotionEmissionTests.cpp
        game/party/PotionFlightTests.cpp
        game/rollback/PlayerActionCheckpointTests.cpp
        game/rollback/PlayerTurboCheckpointTests.cpp
        game/rollback/AnimationCheckpointTests.cpp
        game/rollback/RandomCheckpointTests.cpp
        game/enemies/EnemyMissilesPacketTests.cpp
        game/rollback/EnemyPoolCheckpointTests.cpp
        game/rollback/GeneratorCheckpointTests.cpp
        game/rollback/PlacedEnemyCheckpointTests.cpp
        game/enemies/OpponentPlacementsTests.cpp
        game/party/PlayerAttacksCheckpointTests.cpp
        game/party/PlayerRuntimeCheckpointTests.cpp
        game/enemies/CombatRollbackCheckpointTests.cpp
        game/rollback/PlayerRuntimePacketTests.cpp
        game/rollback/CombatStatePacketTests.cpp
        game/rollback/PlayerHealthCheckpointTests.cpp
        game/enemies/OpponentCombatCheckpointTests.cpp
        game/enemies/OpponentLootCheckpointTests.cpp
        game/rollback/StageStatePacketTests.cpp
        game/rollback/CanonicalDigestSinkTests.cpp
        game/party/PlayerCompanionCheckpointTests.cpp
        game/party/PartyLightingTests.cpp
        game/critters/CombatantCheckpointTests.cpp
        game/critters/CrittersCheckpointTests.cpp
        game/critters/CombatantProjectilesCheckpointTests.cpp
        game/level/ExitPortalsCheckpointTests.cpp
        game/level/LevelTransportersCheckpointTests.cpp
        game/enemies/CombatResourcesTests.cpp
        game/level/ChestsCheckpointTests.cpp
        game/level/LockedGatesCheckpointTests.cpp
        game/level/BreakablesCheckpointTests.cpp
        game/level/SafeRocksCheckpointTests.cpp
        game/level/StaticSceneryCheckpointTests.cpp
        game/level/RubbleCheckpointTests.cpp
        game/level/TrapsCheckpointTests.cpp
        game/level/FallingSceneryCheckpointTests.cpp
        game/level/DestructibleWallsCheckpointTests.cpp
        game/level/RotatorsCheckpointTests.cpp
        game/level/RotatorsLocalRewindTests.cpp
        game/level/PlacedItemsCheckpointTests.cpp
        game/level/PreparedPickupBirthTests.cpp
        game/level/LevelFixturesCheckpointTests.cpp
        game/level/LevelWorldCheckpointTests.cpp
        game/camera/TowerCameraCheckpointTests.cpp
        engine/world/WorldStateCheckpointTests.cpp
        engine/world/WorldCollapsedCollisionTests.cpp
        engine/world/WorldCollisionTests.cpp
        engine/world/ParticleFieldTests.cpp
        game/level/WorldDestructionCheckpointTests.cpp
        game/enemies/EnemyFeedbackEmissionTests.cpp
        game/level/LevelTriggersCheckpointTests.cpp
        game/party/PartyDamageTests.cpp
        game/hud/HelpMessagesTests.cpp
        game/party/PlayerArsenalCheckpointTests.cpp
        game/party/MoveStrikesCheckpointTests.cpp
        game/rollback/EffectIdentityTests.cpp
        game/rollback/PlayerPoseCheckpointTests.cpp
        game/rollback/PlayerPotionCheckpointTests.cpp
        engine/world/ParticleCheckpointTests.cpp
        game/effects/EffectCheckpointTests.cpp
        game/level/WorldMotionTests.cpp
        game/party/TurboEmissionTests.cpp
        game/party/TurboEffectResourcesTests.cpp
        game/rollback/TurboEffectCheckpointTests.cpp
        game/party/PlayerMissilesTests.cpp
        game/party/PlayerMissilesCheckpointTests.cpp
        game/effects/EffectTreesPoolCheckpointTests.cpp
        game/party/MeleeGeometryTests.cpp
        game/rollback/PlayerKnockbackCheckpointTests.cpp
        game/players/KnockbackTests.cpp
        game/players/PlayerActorCheckpointTests.cpp
        game/enemies/EnemyAnimatorCheckpointTests.cpp
        game/enemies/EnemiesCheckpointTests.cpp
        engine/math/Mt19937Tests.cpp
        engine/world/AmbientDimmerTests.cpp
        game/enemies/GeneratorsCheckpointTests.cpp
        game/enemies/EnemyMissilesCheckpointTests.cpp
        game/enemies/PlacedEnemiesTests.cpp
        game/critters/CritterStatuesCheckpointTests.cpp
        game/enemies/OpponentSimulationTests.cpp
        game/enemies/OpponentResourcesTests.cpp
        game/enemies/CombatProgressCheckpointTests.cpp
        game/enemies/LevelOpponentsTests.cpp
        game/enemies/LevelOpponentsDeathTests.cpp
        game/enemies/EnemyMissilesTests.cpp
        game/enemies/GeneratorsTests.cpp
        game/players/TurboMeterTests.cpp game/players/TurboMoveTests.cpp
        game/players/MeleeSelectionTests.cpp
        engine/world/AnimationPlayerTests.cpp engine/world/WorldAnimatorTests.cpp
        game/players/PlayerAnimatorTests.cpp
        game/netplay/MeasuredPacketTransportTests.cpp
        game/online/OnlineCompletionTests.cpp
        game/online/OnlineLevelEntryTests.cpp
        engine/audio/SoundPlayerTests.cpp
        game/app/OnlineRunTests.cpp game/netplay/NativeCampaignTransportTests.cpp
        game/scenes/TitleSceneTests.cpp
        game/rollback/InputCommandTests.cpp
        game/rollback/InputTimelineTests.cpp game/rollback/SessionInputsTests.cpp
        game/netplay/MatchSessionTests.cpp game/netplay/ShopSyncTests.cpp game/players/CursorAimTests.cpp
        game/party/PartyMotionTests.cpp game/party/PartyFiguresTests.cpp
        game/party/FloorRidingTests.cpp
        game/party/PlayerAttacksTests.cpp game/enemies/EnemiesTests.cpp
        fixtures/NativeModelFixture.cpp fixtures/ReferenceObj.cpp fixtures/NativeSoundBank.cpp)
    target_link_libraries(netplayplaytests PRIVATE game_diagnostics gdl::game Catch2::Catch2WithMain)
    target_include_directories(netplayplaytests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(netplayplaytests PRIVATE
        GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}"
        GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
    gdl_apply_compiler_options(netplayplaytests)
    if(MSVC)
        target_link_options(netplayplaytests PRIVATE /STACK:16777216) # as the suite's, below
    endif()
    set_target_properties(netplayplaytests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
    # Actual gameplay, independent-process determinism observations; no replicas.
    add_executable(nativeprobe game/rollback/NativeSimulationProbe.cpp game/rollback/NativeProbeLink.cpp
        game/rollback/SimulationObserver.cpp game/rollback/SimulationObserverBindings.cpp)
    target_link_libraries(nativeprobe PRIVATE game_diagnostics gdl::game gdl::mods gdl::engine_net)
    target_include_directories(nativeprobe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(nativeprobe PRIVATE
        GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}" GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
    gdl_apply_compiler_options(nativeprobe)
    set_target_properties(nativeprobe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
    target_sources(netplayplaytests PRIVATE game/level/SimulationSeedTests.cpp
        game/rollback/SimulationObserver.cpp game/rollback/SimulationObserverBindings.cpp
        game/scenes/NativePresentationReadinessTests.cpp
        game/rollback/HintInputCommandTests.cpp game/rollback/NativeHintInputTests.cpp
        game/netplay/NativeAgreementSceneTests.cpp
        game/netplay/NativeAgreementTests.cpp
        game/netplay/OnlineNativeBootstrapTests.cpp game/online/NativeOnlinePlayTests.cpp
        game/app/NativeOnlineIdentityTests.cpp game/app/NumericWitnessTests.cpp
        game/rollback/SimulationObservationTests.cpp game/scenes/PlaySceneSimulationCameraTests.cpp
        game/rollback/LockstepInputsTests.cpp game/rollback/LockstepInputs.cpp game/rollback/MatchLockstepTests.cpp
        game/online/NativeRollbackLifecycleTests.cpp game/online/NativePlaySetupTests.cpp
        game/audio/SpeechTimingTests.cpp)
    add_executable(simulationtests game/level/SimulationSeedTests.cpp
        engine/io/NativeStateDigestTests.cpp
        engine/io/Sha256Tests.cpp game/rollback/CanonicalDigestSinkTests.cpp
        engine/world/WorldCollisionTests.cpp engine/world/WorldAnimatorTests.cpp
        engine/world/WorldSceneTests.cpp
        game/party/PlayerNativeStateTests.cpp
        game/enemies/EnemyMissilesNativeDigestTests.cpp
        game/critters/CombatantProjectilesNativeDigestTests.cpp
        game/level/LevelTriggersNativeDigestTests.cpp
        game/level/SecretChallengeNativeDigestTests.cpp
        game/level/NativeStateResourcesTests.cpp
        engine/assets/MessageTableTests.cpp
        engine/assets/BitmapFontTests.cpp
        engine/ui/TextPainterTests.cpp
        game/enemies/GeneratorNativeDigestTests.cpp
        game/enemies/EnemiesNativeDigestTests.cpp
        game/critters/CombatantNativeDigestTests.cpp
        game/enemies/PlacedEnemiesNativeDigestTests.cpp
        game/enemies/OpponentOwnersNativeDigestTests.cpp
        game/critters/CritterStatuesNativeDigestTests.cpp
        game/camera/CameraMeterNativeDigestTests.cpp
        game/effects/EffectTreesNativeDigestTests.cpp
        game/party/PlayerAttackNativeDigestTests.cpp
        game/party/PlayerFigureNativeDigestTests.cpp
        game/level/LevelWorldNativeDigestTests.cpp
        game/level/LevelFixturesNativeDigestTests.cpp
        game/audio/SceneAudioNativeDigestTests.cpp
        game/scenes/SceneTravelNativeDigestTests.cpp
        game/hud/SceneHudNativeDigestTests.cpp
        game/scenes/PlaySceneNativeStateDigestTests.cpp
        game/rollback/EnemyPoolCheckpointTests.cpp
        game/enemies/EnemyMissilesPacketTests.cpp
        game/rollback/PlacedEnemyCheckpointTests.cpp
        game/enemies/EnemiesDeathTests.cpp game/enemies/EnemyAnimatorTests.cpp
        game/enemies/LevelOpponentsDeathTests.cpp
        engine/world/AnimationPlayerTests.cpp
        game/scenes/PlaySceneLocalRewindTests.cpp
        game/scenes/PlaySceneOutputTests.cpp
        game/rollback/SimulationObserverBossTests.cpp
        game/rollback/NativeRollbackSceneTests.cpp
        game/online/NativeRollbackDriverTests.cpp
        game/scenes/PlaySceneControlEqualityTests.cpp
        game/audio/SceneSoundEventsTests.cpp
        game/level/TriggerCuesTests.cpp
        game/party/PlayerAttacksCheckpointTests.cpp
        game/party/MoveStrikesCheckpointTests.cpp
        game/enemies/OpponentCombatCheckpointTests.cpp
        game/enemies/EnemyMissilesCheckpointTests.cpp
        game/enemies/PlacedEnemiesTests.cpp
        game/critters/CritterStatuesCheckpointTests.cpp
        game/hud/BossMeterTests.cpp
        game/level/ExitPortalsCheckpointTests.cpp
        game/level/LevelTransportersCheckpointTests.cpp
        game/party/PortalDepartureTests.cpp
        game/tower/LevelArrivalPresentationTests.cpp
        game/tower/SumnerFigureTests.cpp
        game/hud/PartyHudTests.cpp
        game/hud/ChallengeHudTests.cpp
        game/hud/LevelMessagesTests.cpp
        game/tower/SumnerVisitTests.cpp
        game/widgets/ScrollBoxTests.cpp
        game/tower/HintMenuTests.cpp
        game/widgets/BurnDialogueScrollTests.cpp
        game/tower/SumnerHintsTests.cpp
        game/tower/TowerPromotionTests.cpp
        game/bosses/BossVictoryPresentationTests.cpp
        game/bosses/LegendPresentationTests.cpp
        engine/world/WorldStateCheckpointTests.cpp
        game/level/PlacedItemsCheckpointTests.cpp game/level/PlacedItemsTests.cpp
        game/level/SafeRocksCheckpointTests.cpp game/level/RubbleCheckpointTests.cpp
        game/level/LevelTriggersCheckpointTests.cpp
        game/level/LevelWorldCheckpointTests.cpp game/level/LevelFixturesCheckpointTests.cpp
        game/level/FallingSceneryCheckpointTests.cpp game/level/DestructibleWallsCheckpointTests.cpp
        game/level/WorldDestructionCheckpointTests.cpp game/level/SkorneArenaTests.cpp
        game/enemies/GeneratorsTests.cpp game/enemies/GeneratorsCheckpointTests.cpp
        game/critters/CombatantLocalRewindTests.cpp game/critters/CombatantCheckpointTests.cpp
        game/critters/CrittersLocalRewindTests.cpp game/critters/CrittersCheckpointTests.cpp
        game/bosses/BossLocalRewindTests.cpp
        engine/world/TreePoseTests.cpp
        engine/world/ParticleLocalRewindTests.cpp engine/world/ParticleSystemTests.cpp
        game/effects/EffectTreesLocalRewindTests.cpp game/effects/EffectTreesTests.cpp
        game/level/ItemFigureLocalRewindTests.cpp
        game/level/ChestsCheckpointTests.cpp game/level/LockedGatesCheckpointTests.cpp
        game/level/BreakablesCheckpointTests.cpp game/level/TrapsCheckpointTests.cpp
        game/level/RotatorsCheckpointTests.cpp game/level/StaticSceneryCheckpointTests.cpp
        game/level/RotatorsLocalRewindTests.cpp
        game/effects/EffectTreesPoolCheckpointTests.cpp game/effects/EffectCheckpointTests.cpp
        game/party/PlayerMissilesTests.cpp game/party/PlayerMissilesCheckpointTests.cpp
        game/critters/CombatantProjectilesTests.cpp game/critters/CombatantProjectilesCheckpointTests.cpp
        game/tower/TowerRelicsTests.cpp
        game/camera/TowerCameraTests.cpp game/camera/TowerCameraCheckpointTests.cpp
        game/party/PowerupCompanionTests.cpp game/party/PlayerFamiliarTests.cpp
        game/rollback/LocalStateHistoryTests.cpp
        game/netplay/RollbackExchangeTests.cpp
        game/audio/NativeSoundIdentityTests.cpp
        game/rollback/RollbackInputsTests.cpp
        game/rollback/FrameEventLedgerTests.cpp
        game/party/PlayerLocalRewindTests.cpp
        game/party/PlayerSimulationEqualityTests.cpp
        game/party/PlayerRuntimeCheckpointTests.cpp
        game/party/PlayerCompanionCheckpointTests.cpp game/party/PlayerFigureTests.cpp
        game/party/PartyMotionTests.cpp game/party/PartyFiguresTests.cpp
        game/players/PlayerActorCheckpointTests.cpp
        game/rollback/NativeProbeTimingTests.cpp
        game/enemies/EnemiesCheckpointTests.cpp engine/world/TextureAnimatorTests.cpp
        game/rollback/SimulationObserver.cpp game/rollback/SimulationObserverBindings.cpp
        game/scenes/NativePresentationReadinessTests.cpp
        engine/app/ApplicationTimingTests.cpp
        game/rollback/InputCommandTests.cpp
        game/rollback/HintInputCommandTests.cpp game/rollback/NativeHintInputTests.cpp
        game/netplay/NativeAgreementSceneTests.cpp
        game/netplay/NativeAgreementTests.cpp
        game/netplay/OnlineSessionTests.cpp game/netplay/OnlineNativeBootstrapTests.cpp
        game/online/NativeOnlinePlayTests.cpp game/app/OnlineRunTests.cpp
        game/netplay/NativeCampaignTransportTests.cpp
        game/netplay/OnlinePartyTests.cpp
        game/app/NativeOnlineIdentityTests.cpp game/app/NumericWitnessTests.cpp engine/assets/StringTableTests.cpp
        game/rollback/SimulationObservationTests.cpp game/scenes/PlaySceneSimulationCameraTests.cpp
        game/rollback/LockstepInputsTests.cpp game/rollback/LockstepInputs.cpp game/rollback/MatchLockstepTests.cpp
        game/netplay/MatchSessionTests.cpp game/netplay/ShopSyncTests.cpp game/shop/ShopTests.cpp
        game/scenes/AfterLevelSceneTests.cpp
        game/online/NativeRollbackLifecycleTests.cpp game/online/NativePlaySetupTests.cpp
        game/audio/SpeechTimingTests.cpp
        game/audio/VoiceQueueTests.cpp game/audio/LevelSoundscapeTests.cpp
        game/bosses/BossSequenceTests.cpp fixtures/NativeSoundBank.cpp fixtures/NativeModelFixture.cpp
        fixtures/ReferenceObj.cpp game/party/PlayerAttacksTests.cpp game/party/PartyPickupsTests.cpp
        game/scenes/PlaySceneTests.cpp engine/math/Mt19937Tests.cpp)
    target_link_libraries(simulationtests PRIVATE game_diagnostics gdl::game Catch2::Catch2WithMain)
    target_include_directories(simulationtests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(simulationtests PRIVATE
        GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}" GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
    gdl_apply_compiler_options(simulationtests)
    if(MSVC)
        target_link_options(simulationtests PRIVATE /STACK:16777216) # as the suite's, below
    endif()
    set_target_properties(simulationtests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
endif()
target_sources(tests PRIVATE engine/io/Sha256Tests.cpp game/netplay/AssetDigestTests.cpp
    game/scenes/PlaySceneLocalRewindTests.cpp
    game/scenes/PlaySceneOutputTests.cpp
    game/rollback/SimulationObserverBossTests.cpp
    game/rollback/NativeRollbackSceneTests.cpp
    game/online/NativeRollbackDriverTests.cpp
    game/scenes/PlaySceneControlEqualityTests.cpp
    game/audio/SceneSoundEventsTests.cpp
    game/rollback/SimulationObserver.cpp game/rollback/SimulationObserverBindings.cpp
    game/scenes/NativePresentationReadinessTests.cpp
    game/rollback/HintInputCommandTests.cpp game/rollback/NativeHintInputTests.cpp
    game/online/NetplayMenuTests.cpp
    game/level/SimulationSeedTests.cpp game/rollback/SimulationObservationTests.cpp
    game/scenes/PlaySceneSimulationCameraTests.cpp game/online/NativePlaySetupTests.cpp
    game/online/NativeRollbackLifecycleTests.cpp game/rollback/LockstepInputsTests.cpp game/rollback/LockstepInputs.cpp
    game/netplay/NativeAgreementSceneTests.cpp
    game/rollback/MatchLockstepTests.cpp game/netplay/NativeAgreementTests.cpp
    game/netplay/OnlineNativeBootstrapTests.cpp game/online/NativeOnlinePlayTests.cpp
    game/app/NativeOnlineIdentityTests.cpp game/app/NumericWitnessTests.cpp game/audio/SpeechTimingTests.cpp)
target_link_libraries(tests PRIVATE Vulkan::Headers volk::volk GPUOpen::VulkanMemoryAllocator)
target_link_libraries(tests PRIVATE gdl::expansion_formats gdl::expansion_audio)
target_sources(tests PRIVATE
    expansions/ExpansionIsolationTests.cpp
    expansions/shared/audio/RestoredSpeechTests.cpp
    expansions/n64/audio/LegendsAnnouncementsTests.cpp
    expansions/n64/audio/LegendsAnnouncementIntegrationTests.cpp
    expansions/n64/audio/MortBankTests.cpp)
if(GDL_LEGENDS_AUDIO)
    target_sources(tests PRIVATE
        expansions/n64/audio/N64LegendsStreamTests.cpp
        expansions/n64/audio/N64SongTimingTests.cpp
        expansions/n64/audio/N64LegendsEffectsTests.cpp
        expansions/n64/audio/LegendsSoundsTests.cpp
        expansions/n64/audio/LegendsSpeechTests.cpp
        expansions/n64/scenes/LegendsMotionSoundsTests.cpp)
    target_link_libraries(tests PRIVATE gdl::legends_audio_native)
    gdl_configure_legends_audio_source(expansions/n64/audio/N64LegendsEffectsTests.cpp)
    gdl_configure_legends_audio_source(expansions/n64/audio/N64SongTimingTests.cpp)
    # Sequence timing is independent of gameplay and installed game assets.
    add_executable(legends_song_timing_tests expansions/n64/audio/N64SongTimingTests.cpp)
    target_link_libraries(legends_song_timing_tests PRIVATE
        Catch2::Catch2WithMain gdl::legends_audio_native)
    gdl_apply_compiler_options(legends_song_timing_tests)
    set_target_properties(legends_song_timing_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>" FOLDER "tests")
    add_test(NAME legends_song_timing COMMAND legends_song_timing_tests)
else()
    target_sources(tests PRIVATE expansions/n64/audio/N64LegendsDisabledTests.cpp)
endif()
target_include_directories(tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tests PRIVATE
    VK_NO_PROTOTYPES
    GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}"
    GDL_TEST_UNPACKED_DIR="${GDL_UNPACKED_DIR}"
    GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")

gdl_apply_compiler_options(tests)
# The enemy pool is tens of kilobytes a copy and the rewind tests hold many: MSVC's
# megabyte of stack is not enough, where the other platforms' eight is.
if(MSVC)
    target_link_options(tests PRIVATE /STACK:16777216)
endif()

# Shares bin/ with the game so the compiled shaders and dependency DLLs are found.
set_target_properties(tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    FOLDER "tests")
add_dependencies(tests gauntlet_shaders expansion_audio_tests)

# Unit tests are discovered individually; the GPU integration tests run as one
# CTest entry labelled "gpu" so machines without a display can skip them
# with `ctest -LE gpu`. Tests tagged [assets] skip themselves when the game
# data is not installed.
set(GDL_TEST_DISCOVERY_FILTER "~[gpu]" CACHE STRING
    "Catch2 discovery filter (public release runners exclude private asset tests)")
catch_discover_tests(tests TEST_SPEC "${GDL_TEST_DISCOVERY_FILTER}" TEST_PREFIX "unit.")
add_test(NAME gpu.integration COMMAND tests "[gpu]")
set_tests_properties(gpu.integration PROPERTIES LABELS gpu)
