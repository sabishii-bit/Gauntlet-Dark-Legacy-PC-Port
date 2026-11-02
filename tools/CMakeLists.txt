find_package(Stb REQUIRED)

# vqdump: decodes a VQ movie to PNG frames and a WAV file, for checking the codec without the game.
add_executable(vqdump
    vqdump/main.cpp
    vqdump/StbImplementation.cpp)

target_link_libraries(vqdump PRIVATE gdl::engine)
target_include_directories(vqdump SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
gdl_apply_compiler_options(vqdump)
gdl_use_utf8_code_page(vqdump)

# gdlunpack: converts the console asset files (objects.ngc, textures.ngc, *.fnt, *.rom)
# into PNG images and JSON manifests under assets/unpacked, which the game loads.
add_executable(gdlunpack
    gdlunpack/main.cpp
    vqdump/StbImplementation.cpp)

target_link_libraries(gdlunpack PRIVATE gdl::formats gdl::engine)
target_include_directories(gdlunpack SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
gdl_apply_compiler_options(gdlunpack)
gdl_use_utf8_code_page(gdlunpack)

find_package(nlohmann_json CONFIG REQUIRED)
add_executable(assetcheck assetcheck/main.cpp assetcheck/DataAudit.cpp)
target_link_libraries(assetcheck PRIVATE gdl::game nlohmann_json::nlohmann_json)
gdl_apply_compiler_options(assetcheck)
gdl_use_utf8_code_page(assetcheck)

set_target_properties(vqdump gdlunpack assetcheck PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    FOLDER "tools")

# Optional source data is supplied at runtime, never required to build the game.
add_executable(mortdump mortdump/main.cpp)
target_link_libraries(mortdump PRIVATE gdl::expansion_audio gdl::formats)
gdl_apply_compiler_options(mortdump)
gdl_use_utf8_code_page(mortdump)
set_target_properties(mortdump PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>" FOLDER "tools")

if(GDL_ENABLE_NETPLAY)
    find_package(Threads REQUIRED)
    # Deliberately independent of gdl::game/engine: no window, renderer or assets.
    add_executable(netplaycheck netplaycheck/main.cpp
        ${CMAKE_SOURCE_DIR}/src/game/rollback/InputCommand.cpp
        ${CMAKE_SOURCE_DIR}/src/game/rollback/InputTimeline.cpp)
    target_link_libraries(netplaycheck PRIVATE gdl::engine_net nlohmann_json::nlohmann_json
        Threads::Threads)
    gdl_apply_compiler_options(netplaycheck)
    gdl_use_utf8_code_page(netplaycheck)
    set_target_properties(netplaycheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>" FOLDER "tools")
endif()
