# Everything except main() lives in a library so the tests can link it.
add_library(game STATIC
    CommandLine.h
    CommandLine.cpp
    AttractSequencer.h
    AttractSequencer.cpp
    MovieScene.h
    MovieScene.cpp
    Gauntlet.h
    Gauntlet.cpp
    SmokeTestScene.h
    SmokeTestScene.cpp)

add_library(gdl::game ALIAS game)

target_link_libraries(game PUBLIC gdl::engine)
gdl_apply_compiler_options(game)
set_target_properties(game PROPERTIES FOLDER "game")

add_executable(gauntlet main.cpp)

target_link_libraries(gauntlet PRIVATE gdl::game)

target_compile_definitions(gauntlet PRIVATE
    GDL_DEFAULT_ASSET_DIR="${GDL_ASSET_DIR}")

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
