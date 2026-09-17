# Everything except main() lives in a library so the tests can link it.
# Modules, lowest first: config (settings), menu (widgets), screens (full screens and
# what they receive), app (the driver and attract flow). A module only includes those
# below it.
add_library(game STATIC
    app/Gauntlet.h
    app/Gauntlet.cpp
    app/CommandLine.h
    app/CommandLine.cpp
    app/AttractSequencer.h
    app/AttractSequencer.cpp
    config/GameConfig.h
    config/GameConfig.cpp
    menu/MenuInput.h
    menu/MenuInput.cpp
    menu/OptionMenu.h
    menu/OptionMenu.cpp
    menu/FireScroll.h
    menu/FireScroll.cpp
    screens/GameContext.h
    screens/TitleScene.h
    screens/TitleScene.cpp
    screens/MovieScene.h
    screens/MovieScene.cpp
    screens/SmokeTestScene.h
    screens/SmokeTestScene.cpp)

add_library(gdl::game ALIAS game)

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(game PUBLIC gdl::engine PRIVATE nlohmann_json::nlohmann_json)
gdl_apply_compiler_options(game)
set_target_properties(game PROPERTIES FOLDER "game")

add_executable(gauntlet main.cpp)

target_link_libraries(gauntlet PRIVATE gdl::game)

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
