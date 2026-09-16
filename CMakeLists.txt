cmake_minimum_required(VERSION 3.30)

project(GauntletDarkLegacy
    VERSION 0.1.0
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

set(GDL_ASSET_DIR "${CMAKE_SOURCE_DIR}/assets/GUNE5D/Gauntlet" CACHE PATH
    "Game asset directory (the GUNE5D/Gauntlet tree)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(GdlCompilerOptions)
include(GdlShaders)

add_subdirectory(src/engine)
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
