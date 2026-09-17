# Everything except main() lives in a library so the tests can link it.
# Modules, lowest first: config (settings), players (class data, progression and saves),
# menu (widgets), screens (full screens and what they receive), app (the driver and attract
# flow). A module only includes those below it.
add_library(game STATIC
    app/Gauntlet.h
    app/Gauntlet.cpp
    app/CommandLine.h
    app/CommandLine.cpp
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
    menu/MenuInput.h
    menu/MenuInput.cpp
    menu/OptionMenu.h
    menu/OptionMenu.cpp
    menu/FireScroll.h
    menu/FireScroll.cpp
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
    screens/PlayerSelectScene.h
    screens/PlayerSelectScene.cpp)

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
