find_package(Stb REQUIRED)

# vqdump: decodes a VQ movie to PNG frames and a WAV file, for checking the codec without the game.
add_executable(vqdump
    vqdump/main.cpp
    vqdump/StbImplementation.cpp)

target_link_libraries(vqdump PRIVATE gdl::engine)
target_include_directories(vqdump SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
gdl_apply_compiler_options(vqdump)

set_target_properties(vqdump PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    FOLDER "tools")
