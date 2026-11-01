# A small headless decoder/backend executable, independent of the game and its
# scene-test roster. This also exercises the dependency-free OFF configuration.
add_executable(expansion_audio_tests
    ../n64/audio/MortBankTests.cpp
    ../shared/audio/RestoredSpeechTests.cpp)
if(NOT GDL_LEGENDS_AUDIO)
    target_sources(expansion_audio_tests PRIVATE ../n64/audio/N64LegendsDisabledTests.cpp)
endif()
target_link_libraries(expansion_audio_tests PRIVATE
    gdl::expansion_audio Catch2::Catch2WithMain)
target_include_directories(expansion_audio_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(expansion_audio_tests PRIVATE
    GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}"
    GDL_TEST_UNPACKED_DIR="${GDL_UNPACKED_DIR}"
    GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
gdl_apply_compiler_options(expansion_audio_tests)
set_target_properties(expansion_audio_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    FOLDER "tests/expansions")

# Native corpus coverage is opt-in via this executable's [assets] filter. The
# default smoke needs no game data, window, renderer or optional sound backend.
add_test(NAME expansion_audio_smoke COMMAND expansion_audio_tests "~[assets]" --reporter compact)
set_tests_properties(expansion_audio_smoke PROPERTIES LABELS "expansions;audio")
