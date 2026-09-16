# GLSL -> SPIR-V at build time, using glslc (Vulkan SDK) or glslang (vcpkg).
#
#   gdl_add_shaders(<target> OUTPUT_DIR <dir> SOURCES <file.vert> <file.frag> ...)
#
# Produces <dir>/<file>.spv for every source and makes <target> depend on them.

set(_gdl_shader_hints "$ENV{VULKAN_SDK}/Bin")
foreach(_dir IN ITEMS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/glslang"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/glslang")
    list(APPEND _gdl_shader_hints "${_dir}")
endforeach()

find_program(GDL_GLSLC_EXECUTABLE NAMES glslc HINTS ${_gdl_shader_hints})
find_program(GDL_GLSLANG_EXECUTABLE NAMES glslang glslangValidator HINTS ${_gdl_shader_hints})

if(GDL_GLSLC_EXECUTABLE)
    set(GDL_GLSL_COMPILER "${GDL_GLSLC_EXECUTABLE}")
    set(GDL_GLSL_COMPILER_KIND "glslc")
elseif(GDL_GLSLANG_EXECUTABLE)
    set(GDL_GLSL_COMPILER "${GDL_GLSLANG_EXECUTABLE}")
    set(GDL_GLSL_COMPILER_KIND "glslang")
else()
    message(FATAL_ERROR
        "No GLSL compiler found. Install the LunarG Vulkan SDK (glslc) or let vcpkg build "
        "glslang[tools] from vcpkg.json. Searched: ${_gdl_shader_hints}")
endif()
message(STATUS "GLSL compiler: ${GDL_GLSL_COMPILER} (${GDL_GLSL_COMPILER_KIND})")

function(gdl_add_shaders target)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR" "SOURCES" ${ARGN})
    if(NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR "gdl_add_shaders: OUTPUT_DIR is required")
    endif()

    set(_outputs)
    foreach(_src IN LISTS ARG_SOURCES)
        get_filename_component(_name "${_src}" NAME)
        set(_out "${ARG_OUTPUT_DIR}/${_name}.spv")
        if(GDL_GLSL_COMPILER_KIND STREQUAL "glslc")
            set(_cmd "${GDL_GLSL_COMPILER}" --target-env=vulkan1.3 -o "${_out}" "${_src}")
        else()
            set(_cmd "${GDL_GLSL_COMPILER}" -V --target-env vulkan1.3 -o "${_out}" "${_src}")
        endif()
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
            COMMAND ${_cmd}
            DEPENDS "${_src}"
            COMMENT "Compiling shader ${_name}"
            VERBATIM)
        list(APPEND _outputs "${_out}")
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${_outputs} SOURCES ${ARG_SOURCES})
    set_target_properties(${target}_shaders PROPERTIES FOLDER "shaders")
    add_dependencies(${target} ${target}_shaders)
endfunction()
