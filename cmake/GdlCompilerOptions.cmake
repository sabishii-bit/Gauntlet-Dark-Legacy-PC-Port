# gdl_apply_compiler_options(<target>): project-wide warnings, platform defines and the
# floating-point contract the online simulation relies on: IEEE operations as written,
# never contracted into fused multiply-adds, so builds by different compilers agree.
function(gdl_apply_compiler_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /fp:precise
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /EHsc
            /MP
            # Large test units' Debug objects outgrow COFF's default 65,279 sections.
            /bigobj)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -ffp-contract=off
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

# gdl_use_utf8_code_page(<target>): on Windows the executable runs with UTF-8 as its active
# code page (Windows 10 1903 and later), so narrow strings, arguments and
# std::filesystem::path::string() carry every character a path or a name may hold.
function(gdl_use_utf8_code_page target)
    if(WIN32 AND MSVC)
        target_sources(${target} PRIVATE "${CMAKE_SOURCE_DIR}/cmake/utf8.manifest")
    endif()
endfunction()
