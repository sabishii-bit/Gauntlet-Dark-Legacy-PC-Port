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
