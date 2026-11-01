# A mod is a folder under src/mods, built on the game and compiled into the programs. Its
# CMakeLists.txt calls
#
#     gdl_add_mod(<folder> SOURCES <files...>)
#
# and its sources define `void gdl::mods::<folder>::registerMechanics(gdl::game::MechanicCatalog&)`,
# which the build's generated list calls at startup. The folder's name is the mod's namespace:
# lowercase letters, digits and underscores, starting with a letter. Its tests sit in
# tests/mods/<folder> and its text in data/mods/<mechanic id>/text.
function(gdl_add_mod name)
    cmake_parse_arguments(PARSE_ARGV 1 MOD "" "" "SOURCES")
    if(NOT name MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "Mod folder '${name}' must be a lowercase C++ identifier")
    endif()
    if(NOT MOD_SOURCES)
        message(FATAL_ERROR "Mod '${name}' lists no SOURCES")
    endif()
    add_library(mod_${name} STATIC ${MOD_SOURCES})
    target_link_libraries(mod_${name} PUBLIC gdl::game)
    gdl_apply_compiler_options(mod_${name})
    set_target_properties(mod_${name} PROPERTIES FOLDER "mods")
    set_property(GLOBAL APPEND PROPERTY GDL_MODS "${name}")
endfunction()
