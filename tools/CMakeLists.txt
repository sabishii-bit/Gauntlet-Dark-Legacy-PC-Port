find_package(Stb REQUIRED)

# vqdump: decodes a VQ movie to PNG frames and a WAV file, for checking the codec without the game.
add_executable(vqdump
    vqdump/main.cpp
    vqdump/StbImplementation.cpp)

target_link_libraries(vqdump PRIVATE gdl::engine)
target_include_directories(vqdump SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
gdl_apply_compiler_options(vqdump)

# gdlunpack: converts the console asset files (objects.ngc, textures.ngc, *.fnt, *.rom)
# into PNG images and JSON manifests under assets/unpacked, which the game loads.
add_executable(gdlunpack
    gdlunpack/main.cpp
    vqdump/StbImplementation.cpp)

target_link_libraries(gdlunpack PRIVATE gdl::formats gdl::engine)
target_include_directories(gdlunpack SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
gdl_apply_compiler_options(gdlunpack)

find_package(nlohmann_json CONFIG REQUIRED)
add_executable(assetcheck assetcheck/main.cpp assetcheck/DataAudit.cpp)
target_link_libraries(assetcheck PRIVATE gdl::game nlohmann_json::nlohmann_json)
gdl_apply_compiler_options(assetcheck)

set_target_properties(vqdump gdlunpack assetcheck PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    FOLDER "tools")

if(GDL_ENABLE_NETPLAY)
    find_package(lz4 CONFIG REQUIRED)
    find_package(Threads REQUIRED)
    # Deliberately independent of gdl::game/engine: no window, renderer or assets.
    add_executable(netplaycheck netplaycheck/main.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/InputCommand.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/InputTimeline.cpp)
    target_link_libraries(netplaycheck PRIVATE gdl::engine_net nlohmann_json::nlohmann_json
        Threads::Threads)
    gdl_apply_compiler_options(netplaycheck)
    set_target_properties(netplaycheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>" FOLDER "tools")
    add_executable(roomcheck netplaycheck/RoomCheck.cpp netplaycheck/SessionCheck.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/SnapshotBlock.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/world/SceneGeometry.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/MatchSession.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/ClientClock.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/OnlineSession.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/CharacterProfile.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/PartyCheckpoint.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/PartyBootstrap.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/MotionSnapshot.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/SnapshotPlayback.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/CombatSnapshot.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/HudSnapshot.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/FighterSnapshot.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/CombatReplica.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/CombatPlayback.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/InputCommand.cpp
        ${CMAKE_SOURCE_DIR}/src/game/netplay/InputTimeline.cpp)
    target_link_libraries(roomcheck PRIVATE gdl::engine_net gdl::room_service
        nlohmann_json::nlohmann_json Threads::Threads lz4::lz4)
    gdl_apply_compiler_options(roomcheck)
    set_target_properties(roomcheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>" FOLDER "tools")
endif()
