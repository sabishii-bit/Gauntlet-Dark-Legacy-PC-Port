# gdl_test_discovery_specs(<filter> <parallel> <serial>): the Catch2 specs that split the cases a
# discovery filter lists into those ctest may run beside others and those tagged [serial], which
# it runs alone. Each spec joins the tag to the filter's own patterns, which Catch2 ANDs (the
# discovery script passes on only the first of several TEST_SPEC arguments). The split is exact
# only for [tag] and ~[tag] patterns: a comma starts an OR term the tag does not reach, and a ~
# after a name pattern belongs to the name.
function(gdl_test_discovery_specs filter parallel serial)
    if(NOT "${filter}" MATCHES "^( *~?\\[[^],;[\\]+\\])* *$")
        message(FATAL_ERROR "GDL_TEST_DISCOVERY_FILTER takes only [tag] and ~[tag] patterns, "
                            "without names or commas: \"${filter}\"")
    endif()
    set(serial_spec "${filter}[serial]")
    # A spec that requires a tag also lists hidden cases, which a filter of exclusions leaves out.
    if(NOT " ${filter}" MATCHES "[^~]\\[")
        string(APPEND serial_spec "~[.]")
    endif()
    set(${parallel} "${filter}~[serial]" PARENT_SCOPE)
    set(${serial} "${serial_spec}" PARENT_SCOPE)
endfunction()
