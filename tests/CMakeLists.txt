find_package(Catch2 CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
include(Catch)
# gdl_add_debug_reports(<target>): a Catch2 program reports Windows Debug runtime failures on
# stderr and fails instead of opening a dialog. The source is the program's own: a library's
# object that nothing references never reaches the link.
function(gdl_add_debug_reports target)
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests/HeadlessDebugReports.cpp")
endfunction()
add_subdirectory(expansions/audio)

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
    expansions/n64/critters/LegendsBossKindsTests.cpp
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
    game/rollback/LocalOwnerContractTests.cpp
    game/netplay/RollbackExchangeTests.cpp
    game/audio/NativeSoundIdentityTests.cpp
    game/rollback/RollbackInputsTests.cpp
    game/rollback/InputJournalTests.cpp
    game/rollback/BitSet8Tests.cpp
    game/rollback/FrameEventLedgerTests.cpp
    game/party/PlayerLocalRewindTests.cpp
    game/party/PlayerSimulationEqualityTests.cpp
    game/rollback/NativeProbeTimingTests.cpp
    engine/app/ApplicationTimingTests.cpp
    engine/math/Mt19937Tests.cpp
    game/netplay/HostedRoomTests.cpp
    game/rollback/ConsoleActionTests.cpp
    game/rollback/SharedConsoleWordsTests.cpp
    game/rollback/InputCommandTests.cpp
    game/rollback/InputTimelineTests.cpp
    game/rollback/ShotIdentityTests.cpp
    game/enemies/OpponentPlacementsTests.cpp
    game/party/PlayerAttacksLocalRewindTests.cpp
    game/party/PlayerRuntimeLocalRewindTests.cpp
    game/enemies/LevelOpponentsLocalRewindTests.cpp
    game/enemies/OpponentLootCheckpointTests.cpp
    game/rollback/CanonicalDigestSinkTests.cpp
    game/party/PlayerCompanionLocalRewindTests.cpp
    game/party/PartyLightingTests.cpp
    game/level/ExitPortalsLocalRewindTests.cpp
    game/level/LevelTransportersLocalRewindTests.cpp
    game/level/ChestsLocalRewindTests.cpp
    game/level/ChestsTests.cpp
    game/level/LockedGatesLocalRewindTests.cpp
    game/level/LockedGatesTests.cpp
    game/level/BreakablesLocalRewindTests.cpp
    game/level/BreakablesTests.cpp
    game/level/SafeRocksLocalRewindTests.cpp
    game/level/StaticSceneryLocalRewindTests.cpp
    game/level/RubbleLocalRewindTests.cpp
    game/level/TrapsLocalRewindTests.cpp
    game/level/TrapsTests.cpp
    game/level/FallingSceneryLocalRewindTests.cpp
    game/level/DestructibleWallsLocalRewindTests.cpp
    game/level/RotatorsLocalRewindTests.cpp
    game/level/PlacedItemsLocalRewindTests.cpp
    game/level/PreparedPickupBirthTests.cpp
    game/level/LevelFixturesLocalRewindTests.cpp
    game/level/LevelWorldLocalRewindTests.cpp
    game/camera/TowerCameraLocalRewindTests.cpp
    engine/world/WorldLocalRewindTests.cpp
    engine/world/WorldCollapsedCollisionTests.cpp
    game/level/WorldDestructionLocalRewindTests.cpp
    game/enemies/EnemyFeedbackEmissionTests.cpp
    game/party/PartyDamageTests.cpp
    game/party/PlayerArsenalLocalRewindTests.cpp
    game/party/MoveStrikesLocalRewindTests.cpp
    game/rollback/EffectIdentityTests.cpp
    game/level/WorldMotionTests.cpp
    game/party/TurboEmissionTests.cpp
    game/party/TurboEffectResourcesTests.cpp
    game/party/MeleeGeometryTests.cpp
    game/players/MeleeSelectionTests.cpp
    game/enemies/EnemyAnimatorLocalRewindTests.cpp
    game/enemies/EnemiesLocalRewindTests.cpp
    game/enemies/GeneratorsLocalRewindTests.cpp
    game/enemies/EnemyMissilesLocalRewindTests.cpp
    game/enemies/PlacedEnemiesTests.cpp
    game/critters/CritterStatuesLocalRewindTests.cpp
    game/enemies/OpponentSimulationTests.cpp
    game/netplay/MatchSessionTests.cpp
    game/netplay/ShopSyncTests.cpp
    game/online/OnlineCompletionTests.cpp
    game/online/OnlineLevelEntryTests.cpp
    game/online/PartyStoreTests.cpp
    game/online/OnlineLinksTests.cpp
    game/netplay/OnlineSessionTests.cpp
    game/characters/CharacterProfileTests.cpp
    game/characters/PartyCheckpointTests.cpp
    game/netplay/PartyBootstrapTests.cpp
    game/netplay/OnlinePartyTests.cpp
    game/rollback/SessionInputsTests.cpp
    game/app/BuildLabelTests.cpp
    game/app/FrameBorderTests.cpp
    game/app/LevelDebugOverlayTests.cpp
    game/console/DevConsoleTests.cpp
    game/console/ConsolePolicyTests.cpp
    game/console/GotoRequestTests.cpp
    game/console/ConsoleArgumentsTests.cpp
    game/console/ConsoleUsageTests.cpp
    game/console/GiftRequestTests.cpp
    game/console/SpawnRequestTests.cpp
    game/console/FixtureRequestTests.cpp
    game/console/PartyCountRequestTests.cpp
    game/console/LevelListingLinesTests.cpp
    game/console/ConsoleListingsTests.cpp
    game/app/ConsoleActionRouteTests.cpp
    game/app/ConsoleRosterTests.cpp
    game/app/OnlineConsoleCommandsTests.cpp
    game/app/ViewConsoleCommandsTests.cpp
    game/app/InspectConsoleFormTests.cpp
    game/app/PartyCountCommandTests.cpp
    game/app/ProfileConsoleCommandsTests.cpp
    game/scenes/SharedConsoleVerbsTests.cpp
    game/scenes/SharedConsoleCommandTests.cpp
    game/scenes/LevelListingsTests.cpp
    game/scenes/FixtureConsoleCommandsTests.cpp
    game/scenes/ConsoleRepliesTests.cpp
    game/scenes/ConsoleTravelTests.cpp
    game/scenes/SceneDescriptionTests.cpp
    game/app/GameCursorTests.cpp
    game/scenes/PlaySceneRumbleTests.cpp
    game/scenes/HapticsTests.cpp
    game/scenes/ClassUnlocksTests.cpp
    game/scenes/PlaySceneSoundIdentityTests.cpp
    game/level/StaticSceneryTests.cpp
    game/config/ControlProfilesTests.cpp
    game/config/BindingNamesTests.cpp
    game/hud/ControlPromptsTests.cpp
    game/context/GameServicesTests.cpp
    game/players/CursorAimTests.cpp
    game/enemies/DeathRulesTests.cpp
    game/enemies/EnemiesDeathTests.cpp
    game/enemies/LevelOpponentsDeathTests.cpp
    game/scenes/PlaySceneAmbienceTests.cpp
    game/level/DestructibleWallsTests.cpp
    game/combat/MissileTargetTests.cpp
    game/combat/ArmorFlagsTests.cpp
    game/combat/DamageTests.cpp
    game/combat/BodyContactTests.cpp
    game/combat/DamageTypesTests.cpp
    game/combat/EffectFlagsTests.cpp
    game/combat/PowerupFlagsTests.cpp
    game/combat/StrikeHitTests.cpp
    game/combat/CombatArenaTargetTests.cpp
    game/combat/HurtKindTests.cpp
    game/party/PlayerPowerupsTests.cpp
    game/menu/SettingsMenuTests.cpp
    game/menu/LiveSettingsTests.cpp
    game/widgets/MenuFontTests.cpp
    game/menu/SettingsRowsTests.cpp
    game/menu/VideoSettingsTests.cpp
    game/hud/CompassHudTests.cpp
    game/hud/CinematicBarsTests.cpp
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
    TestSupportTests.cpp
    HeadlessDebugReportsTests.cpp
    SyntheticBytes.h
    SyntheticBytesTests.cpp
    NativeStateSupport.h
    NativeStateSupportTests.cpp
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
    engine/core/ExpectedTests.cpp
    engine/core/LocalBindingTests.cpp
    engine/core/ClockTests.cpp
    engine/math/MathTests.cpp
    engine/math/PortableMathTests.cpp
    engine/math/ApproximateLengthTests.cpp
    engine/io/ByteReaderTests.cpp
    engine/io/ByteWriterTests.cpp
    engine/io/FileTests.cpp
    engine/io/AssetLocatorTests.cpp
    engine/io/LocalRosterTests.cpp
    engine/io/LocalStateTests.cpp
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
    expansions/dreamcast/audio/DreamcastSoundBankTests.cpp
    expansions/dreamcast/audio/LegendsDreamcastSoundsTests.cpp
    expansions/dreamcast/audio/LegendsDreamcastSpeechTests.cpp
    expansions/dreamcast/audio/LegendsDreamcastAmbientTests.cpp
    expansions/dreamcast/enemies/LegendsDreamcastEnemiesTests.cpp
    expansions/n64/audio/LegendsOpponentSoundsTests.cpp
    engine/audio/MusicDeclickerTests.cpp
    engine/audio/StreamPlaylistTests.cpp
    engine/assets/ModelSetTests.cpp
    engine/assets/AnimationSetTests.cpp
    engine/ui/ModelSpriteTests.cpp
    engine/world/AmbientDimmerTests.cpp
    engine/world/WorldCameraTests.cpp
    engine/world/WorldSceneTests.cpp
    engine/world/ThermalMaterialTests.cpp
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
    game/party/PlayerMissilesLocalRewindTests.cpp
    game/effects/EffectTreesPoolLocalRewindTests.cpp
    game/effects/EffectTreesLocalRewindTests.cpp
    game/level/ItemFigureLocalRewindTests.cpp
    game/party/TargetAssistTests.cpp
    game/party/BodyCollisionTests.cpp
    game/effects/BlobShadowTests.cpp
    game/effects/ThermalTexturesTests.cpp
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
    expansions/dreamcast/assets/LegendsDreamcastArchiveTests.cpp
    expansions/n64/formats/LegendsN64TextureTests.cpp
    expansions/shared/scenes/LegendsPreviewTests.cpp
    expansions/shared/scenes/LegendsPlayableTests.cpp
    expansions/shared/scenes/LegendsLevelBossTests.cpp
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
    expansions/n64/camera/LegendsCameraChannelTests.cpp
    expansions/n64/camera/LegendsCameraRigTests.cpp
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
    expansions/n64/level/LegendsTowerCeremonyTests.cpp
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
    expansions/shared/level/LegendsContainersTests.cpp
    expansions/shared/level/LegendsGatesTests.cpp
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
    expansions/dreamcast/level/LegendsDreamcastContainersTests.cpp
    expansions/dreamcast/level/LegendsDreamcastGatesTests.cpp
    expansions/dreamcast/camera/LegendsDreamcastCameraTests.cpp
    expansions/dreamcast/camera/LegendsDreamcastArenaCameraTests.cpp
    expansions/dreamcast/camera/LegendsDreamcastCameraBoundsTests.cpp
    expansions/dreamcast/level/LegendsDreamcastPortalsTests.cpp
    expansions/dreamcast/level/LegendsDreamcastPlacementTests.cpp
    expansions/dreamcast/level/LegendsDreamcastFallingTests.cpp
    expansions/dreamcast/level/LegendsDreamcastTowerTests.cpp
    expansions/dreamcast/level/LegendsDreamcastBossCeremonyTests.cpp
    expansions/dreamcast/formats/LegendsDreamcastCritterTests.cpp
    expansions/dreamcast/bosses/LegendsDreamcastBossAssetsTests.cpp
    expansions/dreamcast/bosses/LegendsDreamcastBossEntranceTests.cpp
    expansions/dreamcast/bosses/LegendsDreamcastBossesTests.cpp
    expansions/dreamcast/bosses/LegendsDreamcastBossVisitTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastBossCompletionTests.cpp
    expansions/dreamcast/level/LegendsDreamcastTrapsTests.cpp
    expansions/dreamcast/level/LegendsDreamcastSpikeDamageTests.cpp
    expansions/dreamcast/level/LegendsDreamcastHazardsTests.cpp
    expansions/dreamcast/level/LegendsDreamcastWorldMotionTests.cpp
    expansions/dreamcast/level/LegendsDreamcastSceneryTests.cpp
    expansions/dreamcast/level/LegendsDreamcastArenaRocksTests.cpp
    expansions/dreamcast/level/LegendsDreamcastA4TraversalTests.cpp
    expansions/dreamcast/level/LegendsDreamcastObelisksTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastObelisksTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastArenaTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastContainersTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastEffectsTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastEnemiesTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastFallingTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastGatesTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastRidersTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastSceneryTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastSumnerTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastSwitchesTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastTrapsTests.cpp
    expansions/dreamcast/menu/LegendsDreamcastDialogueTests.cpp
    expansions/dreamcast/menu/LegendsDreamcastEntranceTests.cpp
    expansions/dreamcast/menu/LegendsDreamcastPauseTests.cpp
    expansions/dreamcast/scenes/LegendsDreamcastTravelTests.cpp
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
    game/widgets/PointerTargetsTests.cpp
    game/widgets/SceneInputTests.cpp
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
    game/critters/CombatantProjectilesTests.cpp
    game/critters/CombatantProjectilesLocalRewindTests.cpp
    game/players/PlayerControlsTests.cpp
    game/party/PlayInputReaderTests.cpp
    game/players/PlayerActorTests.cpp
    game/players/PlayerCaptureTests.cpp
    game/players/ComboMoveTests.cpp
    game/party/PartyComboTests.cpp
    game/players/PlayerTransportTests.cpp
    game/players/PlayerAnimatorTests.cpp
    game/players/PlayerAnimatorCadenceTests.cpp
    game/players/PlayerAnimatorInterruptTests.cpp
    game/players/PlayerDeedsTests.cpp
    game/players/ItemAttackTests.cpp
    game/players/KnockbackTests.cpp
    game/players/PlayerImpactTests.cpp
    game/level/LevelWorldTests.cpp
    game/level/WorldLightningTests.cpp
    game/level/BossKindsTests.cpp
    game/level/LevelCatalogTests.cpp
    game/level/LevelItemArchivesTests.cpp
    game/level/TowerAccessTests.cpp
    game/level/InstanceNamesTests.cpp
    game/characters/LevelRecordTests.cpp
    game/level/ExitPortalsTests.cpp
    game/level/LevelTransportersTests.cpp
    game/scenes/PlaySceneTransportTests.cpp
    game/scenes/PlaySceneSwitchTests.cpp
    game/level/SwitchCutsceneTests.cpp
    game/level/LevelFixturesFloorsTests.cpp
    game/level/RubbleTests.cpp
    game/level/SafeRocksTests.cpp
    game/level/MissileStopTests.cpp
    game/level/CombatantObstacleTests.cpp
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
    game/critters/CombatantLocalRewindTests.cpp
    game/critters/CrittersLocalRewindTests.cpp
    game/bosses/BossLocalRewindTests.cpp
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
    game/bosses/BossDefinitionTests.cpp
    game/bosses/BossTraitsTests.cpp
    game/bosses/BossesTests.cpp
    game/bosses/LegendItemsTests.cpp
    game/party/MoveStrikesTests.cpp
    game/hud/HelpMessagesTests.cpp
    game/level/LevelTriggersTests.cpp
    game/level/LevelTriggersLocalRewindTests.cpp
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
    game/party/PickupPresentationTests.cpp
    game/level/FixtureHostTests.cpp
    game/level/CombatCoverTests.cpp
    game/level/SafeRockCoverTests.cpp
    game/level/ImportedCoverTests.cpp
    game/level/LevelFixturesTests.cpp
    game/enemies/OpponentHostTests.cpp
    game/enemies/BossEncounterHostTests.cpp
    game/enemies/OpponentPoolTests.cpp
    game/enemies/OpponentCuesTests.cpp
    game/enemies/EncounterLevelTests.cpp
    game/enemies/ImportedOpponentCuesTests.cpp
    game/enemies/LevelOpponentsTests.cpp
    game/party/PlayerHealthTests.cpp
    game/party/AttackHostTests.cpp
    game/party/AttackTargetIdTests.cpp
    game/party/PlayerAttacksTests.cpp
    game/party/PlayerAttacksBlowPathsTests.cpp
    game/menu/NameEntryTests.cpp
    game/scenes/SelectLaneTests.cpp
    game/scenes/PlayerSelectSceneTests.cpp
    formats/PlayerDataWadTests.cpp
    formats/TplFileTests.cpp
    formats/IcoWriterTests.cpp
    game/widgets/BurnDialogueScrollTests.cpp
    engine/assets/StringTableTests.cpp)

target_link_libraries(tests PRIVATE gdl::engine gdl::formats gdl::game Catch2::Catch2WithMain SDL3::SDL3)
gdl_add_debug_reports(tests)
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
target_link_libraries(gameplayregressions PRIVATE gdl::game Catch2::Catch2WithMain)
gdl_add_debug_reports(gameplayregressions)
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
    game/party/PlayerAttacksBlowPathsTests.cpp
    game/party/PartyPickupsTests.cpp game/scenes/PlaySceneTests.cpp
    game/characters/CharacterProfileTests.cpp
    game/party/PowerupCompanionTests.cpp fixtures/NativeModelFixture.cpp
    fixtures/ReferenceObj.cpp fixtures/NativeSoundBank.cpp)
target_link_libraries(playerformtests PRIVATE gdl::game Catch2::Catch2WithMain)
gdl_add_debug_reports(playerformtests)
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
    gdl_add_debug_reports(netplaytests)
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
        game/enemies/OpponentPlacementsTests.cpp
        game/party/PlayerAttacksLocalRewindTests.cpp
        game/party/PlayerRuntimeLocalRewindTests.cpp
        game/enemies/LevelOpponentsLocalRewindTests.cpp
        game/enemies/OpponentLootCheckpointTests.cpp
        game/rollback/CanonicalDigestSinkTests.cpp
        game/party/PlayerCompanionLocalRewindTests.cpp
        game/party/PartyLightingTests.cpp
        game/critters/CombatantProjectilesLocalRewindTests.cpp
        game/level/ExitPortalsLocalRewindTests.cpp
        game/level/LevelTransportersLocalRewindTests.cpp
        game/level/ChestsLocalRewindTests.cpp
        game/level/LockedGatesLocalRewindTests.cpp
        game/level/BreakablesLocalRewindTests.cpp
        game/level/SafeRocksLocalRewindTests.cpp
        game/level/StaticSceneryLocalRewindTests.cpp
        game/level/RubbleLocalRewindTests.cpp
        game/level/TrapsLocalRewindTests.cpp
        game/level/FallingSceneryLocalRewindTests.cpp
        game/level/DestructibleWallsLocalRewindTests.cpp
        game/level/RotatorsLocalRewindTests.cpp
        game/level/PlacedItemsLocalRewindTests.cpp
        game/level/PreparedPickupBirthTests.cpp
        game/level/LevelFixturesLocalRewindTests.cpp
        game/level/LevelWorldLocalRewindTests.cpp
        game/camera/TowerCameraLocalRewindTests.cpp
        engine/world/WorldLocalRewindTests.cpp
        engine/world/WorldCollapsedCollisionTests.cpp
        engine/world/WorldCollisionTests.cpp
        engine/world/ParticleFieldTests.cpp
        game/level/WorldDestructionLocalRewindTests.cpp
        game/enemies/EnemyFeedbackEmissionTests.cpp
        game/level/LevelTriggersLocalRewindTests.cpp
        game/party/PartyDamageTests.cpp
        game/hud/HelpMessagesTests.cpp
        game/party/PlayerArsenalLocalRewindTests.cpp
        game/party/MoveStrikesLocalRewindTests.cpp
        game/rollback/EffectIdentityTests.cpp
        engine/world/ParticleCheckpointTests.cpp
        game/effects/EffectTreesLocalRewindTests.cpp
        game/level/WorldMotionTests.cpp
        game/party/TurboEmissionTests.cpp
        game/party/TurboEffectResourcesTests.cpp
        game/party/PlayerMissilesTests.cpp
        game/party/PlayerMissilesLocalRewindTests.cpp
        game/effects/EffectTreesPoolLocalRewindTests.cpp
        game/party/MeleeGeometryTests.cpp
        game/players/KnockbackTests.cpp
        game/enemies/EnemyAnimatorLocalRewindTests.cpp
        game/enemies/EnemiesLocalRewindTests.cpp
        engine/math/Mt19937Tests.cpp
        engine/world/AmbientDimmerTests.cpp
        game/enemies/GeneratorsLocalRewindTests.cpp
        game/enemies/EnemyMissilesLocalRewindTests.cpp
        game/enemies/PlacedEnemiesTests.cpp
        game/critters/CritterStatuesLocalRewindTests.cpp
        game/enemies/OpponentSimulationTests.cpp
        game/enemies/LevelOpponentsTests.cpp
        game/enemies/LevelOpponentsDeathTests.cpp
        game/enemies/EnemyMissilesTests.cpp
        game/enemies/GeneratorsTests.cpp
        game/players/TurboMeterTests.cpp game/players/TurboMoveTests.cpp
        game/players/MeleeSelectionTests.cpp
        engine/world/AnimationPlayerTests.cpp engine/world/WorldAnimatorTests.cpp
        game/players/PlayerAnimatorTests.cpp
        game/online/OnlineCompletionTests.cpp
        game/online/OnlineLevelEntryTests.cpp
        engine/audio/SoundPlayerTests.cpp
        game/app/OnlineRunTests.cpp game/netplay/NativeCampaignTransportTests.cpp
        game/scenes/TitleSceneTests.cpp
        game/rollback/ConsoleActionTests.cpp game/rollback/SharedConsoleWordsTests.cpp
        game/rollback/InputCommandTests.cpp
        game/rollback/InputTimelineTests.cpp game/rollback/SessionInputsTests.cpp
        game/netplay/MatchSessionTests.cpp game/netplay/ShopSyncTests.cpp game/players/CursorAimTests.cpp
        game/party/PartyMotionTests.cpp game/party/PartyFiguresTests.cpp
        game/party/FloorRidingTests.cpp
        game/party/PlayerAttacksTests.cpp game/enemies/EnemiesTests.cpp
        fixtures/NativeModelFixture.cpp fixtures/ReferenceObj.cpp fixtures/NativeSoundBank.cpp)
    target_link_libraries(netplayplaytests PRIVATE gdl::game Catch2::Catch2WithMain)
    gdl_add_debug_reports(netplayplaytests)
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
    add_executable(nativeprobe game/rollback/NativeSimulationProbe.cpp game/rollback/NativeProbeLink.cpp)
    target_link_libraries(nativeprobe PRIVATE gdl::game gdl::mods gdl::engine_net)
    target_include_directories(nativeprobe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(nativeprobe PRIVATE
        GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}" GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
    gdl_apply_compiler_options(nativeprobe)
    set_target_properties(nativeprobe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
    target_sources(netplayplaytests PRIVATE game/level/SimulationSeedTests.cpp
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
        game/enemies/EnemiesDeathTests.cpp game/enemies/EnemyAnimatorTests.cpp
        game/enemies/LevelOpponentsDeathTests.cpp
        engine/world/AnimationPlayerTests.cpp
        game/scenes/PlaySceneLocalRewindTests.cpp
        game/scenes/PlaySceneConsoleTests.cpp
        game/scenes/ConsoleStageTests.cpp
        game/scenes/PlaySceneOutputTests.cpp
        game/rollback/SimulationObserverBossTests.cpp
        game/rollback/NativeRollbackSceneTests.cpp
        game/online/NativeRollbackDriverTests.cpp
        game/scenes/PlaySceneControlEqualityTests.cpp
        game/audio/SceneSoundEventsTests.cpp
        game/level/TriggerCuesTests.cpp
        game/party/PlayerAttacksLocalRewindTests.cpp
        game/party/MoveStrikesLocalRewindTests.cpp
        game/enemies/LevelOpponentsLocalRewindTests.cpp
        game/enemies/EnemyMissilesLocalRewindTests.cpp
        game/enemies/PlacedEnemiesTests.cpp
        game/critters/CritterStatuesLocalRewindTests.cpp
        game/hud/BossMeterTests.cpp
        game/level/ExitPortalsLocalRewindTests.cpp
        game/level/LevelTransportersLocalRewindTests.cpp
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
        engine/world/WorldLocalRewindTests.cpp
        game/level/PlacedItemsLocalRewindTests.cpp game/level/PlacedItemsTests.cpp
        game/level/SafeRocksLocalRewindTests.cpp game/level/RubbleLocalRewindTests.cpp
        game/level/LevelTriggersLocalRewindTests.cpp
        game/level/LevelWorldLocalRewindTests.cpp game/level/LevelFixturesLocalRewindTests.cpp
        game/level/FallingSceneryLocalRewindTests.cpp game/level/DestructibleWallsLocalRewindTests.cpp
        game/level/WorldDestructionLocalRewindTests.cpp game/level/SkorneArenaTests.cpp
        game/enemies/GeneratorsTests.cpp game/enemies/GeneratorsLocalRewindTests.cpp
        game/critters/CombatantLocalRewindTests.cpp
        game/critters/CrittersLocalRewindTests.cpp
        game/bosses/BossLocalRewindTests.cpp
        engine/world/TreePoseTests.cpp
        engine/world/ParticleLocalRewindTests.cpp engine/world/ParticleSystemTests.cpp
        game/effects/EffectTreesLocalRewindTests.cpp game/effects/EffectTreesTests.cpp
        game/level/ItemFigureLocalRewindTests.cpp
        game/level/ChestsLocalRewindTests.cpp game/level/LockedGatesLocalRewindTests.cpp
        game/level/BreakablesLocalRewindTests.cpp game/level/TrapsLocalRewindTests.cpp
        game/level/StaticSceneryLocalRewindTests.cpp
        game/level/RotatorsLocalRewindTests.cpp
        game/effects/EffectTreesPoolLocalRewindTests.cpp
        game/party/PlayerMissilesTests.cpp game/party/PlayerMissilesLocalRewindTests.cpp
        game/critters/CombatantProjectilesTests.cpp game/critters/CombatantProjectilesLocalRewindTests.cpp
        game/tower/TowerRelicsTests.cpp
        game/camera/TowerCameraTests.cpp game/camera/TowerCameraLocalRewindTests.cpp
        game/party/PowerupCompanionTests.cpp game/party/PlayerFamiliarTests.cpp
        game/rollback/LocalStateHistoryTests.cpp
        game/rollback/LocalOwnerContractTests.cpp
        game/netplay/RollbackExchangeTests.cpp
        game/audio/NativeSoundIdentityTests.cpp game/scenes/PlaySceneSoundIdentityTests.cpp
        game/rollback/RollbackInputsTests.cpp
        game/rollback/FrameEventLedgerTests.cpp
        game/party/PlayerLocalRewindTests.cpp
        game/party/PlayerSimulationEqualityTests.cpp
        game/party/PlayerRuntimeLocalRewindTests.cpp
        game/party/PlayerCompanionLocalRewindTests.cpp game/party/PlayerFigureTests.cpp
        game/party/PartyMotionTests.cpp game/party/PartyFiguresTests.cpp
        game/rollback/NativeProbeTimingTests.cpp
        game/enemies/EnemiesLocalRewindTests.cpp engine/world/TextureAnimatorTests.cpp
        game/scenes/NativePresentationReadinessTests.cpp
        engine/app/ApplicationTimingTests.cpp
        game/rollback/ConsoleActionTests.cpp game/rollback/SharedConsoleWordsTests.cpp
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
    target_link_libraries(simulationtests PRIVATE gdl::game Catch2::Catch2WithMain)
    gdl_add_debug_reports(simulationtests)
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
    game/scenes/PlaySceneConsoleTests.cpp
    game/scenes/ConsoleStageTests.cpp
    game/scenes/PlaySceneOutputTests.cpp
    game/rollback/SimulationObserverBossTests.cpp
    game/rollback/NativeRollbackSceneTests.cpp
    game/online/NativeRollbackDriverTests.cpp
    game/scenes/PlaySceneControlEqualityTests.cpp
    game/audio/SceneSoundEventsTests.cpp
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
    expansions/shared/app/LegendsConsoleWordsTests.cpp
    expansions/shared/app/LegendsCompletionTests.cpp
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
    gdl_add_debug_reports(legends_song_timing_tests)
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
    "Catch2 discovery filter of [tag] and ~[tag] patterns only (public release runners exclude private asset tests)")
# Cases tagged [serial] hold real network peers or audio playback to wall-clock deadlines
# that a machine loaded by `ctest -j` can make them miss, so ctest runs each one alone.
include(GdlTestDiscovery)
gdl_test_discovery_specs("${GDL_TEST_DISCOVERY_FILTER}" gdl_parallel_tests gdl_serial_tests)
catch_discover_tests(tests TEST_SPEC "${gdl_parallel_tests}" TEST_PREFIX "unit.")
catch_discover_tests(tests TEST_SPEC "${gdl_serial_tests}" TEST_PREFIX "unit."
    PROPERTIES RUN_SERIAL TRUE)
add_test(NAME gpu.integration COMMAND tests "[gpu]")
set_tests_properties(gpu.integration PROPERTIES LABELS gpu)
