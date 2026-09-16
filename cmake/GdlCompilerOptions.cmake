# gdl_apply_compiler_options(<target>): project-wide warnings and platform defines.
function(gdl_apply_compiler_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /EHsc
            /MP)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wno-unused-parameter)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target} PRIVATE -fdiagnostics-color=always)
        endif()
    endif()

    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Debug>:GDL_DEBUG=1>
        $<$<NOT:$<CONFIG:Debug>>:GDL_DEBUG=0>)
endfunction()
