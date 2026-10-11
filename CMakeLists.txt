cmake_minimum_required(VERSION 3.30)

option(GDL_ENABLE_NETPLAY "Build the experimental networking transport and harness (requires Rust)" OFF)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GDL_VERSION LIMIT_COUNT 1)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GdlVersion.cmake")
gdl_version_core("${GDL_VERSION}" GDL_VERSION_CORE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(GauntletDarkLegacy
    VERSION "${GDL_VERSION_CORE}"
    DESCRIPTION "C++26 / Vulkan reconstruction of the Gauntlet Dark Legacy engine"
    LANGUAGES CXX)

# MSVC has no /std:c++26 switch yet; CMake reaches its C++26 preview (/std:c++latest) via 23.
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(GDL_ENABLE_VULKAN_VALIDATION
    "Enable the Khronos validation layer when it is available at runtime" ON)
option(GDL_BUILD_TESTS "Build the test suite" ON)
option(GDL_EMBED_DISC_ICON "Embed the user's disc icon (OFF for public releases)" ON)

set(GDL_ASSET_DIR "${CMAKE_SOURCE_DIR}/assets/GUNE5D/Gauntlet" CACHE PATH
    "Game asset directory (the GUNE5D/Gauntlet tree)")
set(GDL_UNPACKED_DIR "${CMAKE_SOURCE_DIR}/assets/unpacked" CACHE PATH
    "Optional gdlunpack output and legacy comparison-test fixtures")
set(GDL_DATA_DIR "${CMAKE_SOURCE_DIR}/data" CACHE PATH
    "Shipped configuration defaults and text tables")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(GdlCompilerOptions)
include(GdlShaders)

add_subdirectory(src/engine)
add_subdirectory(src/formats)
add_subdirectory(src/game)
add_subdirectory(tools)

if(GDL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT gauntlet)

# Keep a copy of the compilation database at the repository root for clangd and clang-tidy.
if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    add_custom_target(compile_commands ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            "${CMAKE_SOURCE_DIR}/compile_commands.json"
        VERBATIM)
    set_target_properties(compile_commands PROPERTIES FOLDER "tooling")
endif()

message(STATUS "GDL asset directory: ${GDL_ASSET_DIR}")
message(STATUS "GDL unpacked directory: ${GDL_UNPACKED_DIR}")
