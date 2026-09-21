find_package(Catch2 CONFIG REQUIRED)
include(Catch)

add_executable(tests
    TestSupport.h
    FakeRenderDevice.h
    engine/core/TypesTests.cpp
    engine/core/LogTests.cpp
    engine/core/AssertTests.cpp
    engine/core/ClockTests.cpp
    engine/math/MathTests.cpp
    engine/io/ByteReaderTests.cpp
    engine/io/FileTests.cpp
    engine/io/AssetLocatorTests.cpp
    engine/platform/InputTests.cpp
    engine/platform/PathsTests.cpp
    engine/render/RenderTypesTests.cpp
    engine/render/ImageTests.cpp
    engine/render/ImmediateBatchTests.cpp
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
    engine/core/StringsTests.cpp
    engine/assets/PngImageTests.cpp
    engine/assets/TextureSetTests.cpp
    engine/assets/BitmapFontTests.cpp
    engine/assets/WavFileTests.cpp
    engine/assets/SoundSetTests.cpp
    engine/audio/SoundPlayerTests.cpp
    engine/audio/AdsStreamTests.cpp
    engine/assets/ObjModelTests.cpp
    engine/assets/ModelSetTests.cpp
    engine/assets/AnimationSetTests.cpp
    engine/ui/ModelSpriteTests.cpp
    engine/world/WorldCameraTests.cpp
    engine/world/WorldSceneTests.cpp
    engine/world/TextureAnimatorTests.cpp
    engine/world/WorldAnimatorTests.cpp
    engine/world/ParticleSystemTests.cpp
    engine/world/ParticleFieldTests.cpp
    engine/world/WorldCollisionTests.cpp
    engine/world/TreeModelTests.cpp
    engine/world/AnimationPlayerTests.cpp
    engine/world/TreePoseTests.cpp
    engine/assets/WorldLayoutTests.cpp
    formats/WorldFileTests.cpp
    formats/WorldDataWadTests.cpp
    engine/assets/WorldDataTests.cpp
    engine/assets/MessageTableTests.cpp
    game/menu/ScrollBoxTests.cpp
    game/world/SumnerFigureTests.cpp
    game/world/SumnerHintsTests.cpp
    game/world/PlayerMissilesTests.cpp
    game/menu/HintMenuTests.cpp
    game/world/PlacedItemsTests.cpp
    game/world/AmbientSoundsTests.cpp
    engine/world/WorldLightingTests.cpp
    engine/ui/CanvasTests.cpp
    engine/ui/TextPainterTests.cpp
    formats/ModelArchiveTests.cpp
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
    game/screens/MovieSceneTests.cpp
    game/menu/MenuInputTests.cpp
    game/menu/OptionMenuTests.cpp
    game/screens/TitleSceneTests.cpp
    game/config/GameConfigTests.cpp
    game/players/ClassDataTests.cpp
    game/players/ProgressionTests.cpp
    game/players/CharacterSaveTests.cpp
    game/players/PartyTests.cpp
    game/players/InventoryTests.cpp
    game/players/ItemPickupTests.cpp
    game/players/PowerupEffectsTests.cpp
    game/screens/PowerupSelectorTests.cpp
    game/screens/TransitionScreenTests.cpp
    game/world/EffectTreesTests.cpp
    game/players/PlayerControlsTests.cpp
    game/players/PlayerActorTests.cpp
    game/players/PlayerAnimatorTests.cpp
    game/world/LevelWorldTests.cpp
    game/world/LevelCatalogTests.cpp
    game/world/ExitPortalsTests.cpp
    game/world/LevelFixturesTests.cpp
    game/screens/HelpMessagesTests.cpp
    game/world/LevelTriggersTests.cpp
    game/world/TowerCameraTests.cpp
    game/world/StartCameraTests.cpp
    game/screens/StatusBoxTests.cpp
    game/screens/PickupHudTests.cpp
    game/screens/PlaySceneTests.cpp
    game/menu/NameEntryTests.cpp
    game/screens/SelectLaneTests.cpp
    game/screens/PlayerSelectSceneTests.cpp
    formats/PlayerDataWadTests.cpp
    formats/TplFileTests.cpp
    formats/IcoWriterTests.cpp
    game/menu/BurnDialogueScrollTests.cpp
    engine/assets/StringTableTests.cpp)

target_link_libraries(tests PRIVATE gdl::engine gdl::formats gdl::game Catch2::Catch2WithMain)
target_include_directories(tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tests PRIVATE
    GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}"
    GDL_TEST_UNPACKED_DIR="${GDL_UNPACKED_DIR}"
    GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")

gdl_apply_compiler_options(tests)

# Shares bin/ with the game so the compiled shaders and dependency DLLs are found.
set_target_properties(tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    FOLDER "tests")
add_dependencies(tests gauntlet_shaders)

# Unit tests are discovered individually; the GPU integration tests run as one
# CTest entry labelled "gpu" so machines without a display can skip them
# with `ctest -LE gpu`. Tests tagged [assets] skip themselves when the game
# data is not installed.
catch_discover_tests(tests TEST_SPEC "~[gpu]" TEST_PREFIX "unit.")
add_test(NAME gpu.integration COMMAND tests "[gpu]")
set_tests_properties(gpu.integration PROPERTIES LABELS gpu)
