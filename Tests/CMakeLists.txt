# Test executables. Each binary uses doctest; CTest labels allow selecting suites:
#   ctest -L unit | integration | stress
function(pf_add_test name label)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE pf_doctest ${PF_TEST_LIBS})
    target_compile_definitions(${name} PRIVATE DOCTEST_CONFIG_NO_POSIX_SIGNALS)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT 600)
endfunction()

set(PF_TEST_LIBS pf_shared)
pf_add_test(pf_unit_tests unit
    Unit/main.cpp
    Unit/test_byteio.cpp
    Unit/test_protocol.cpp
    Unit/test_descriptors.cpp
    Unit/test_identity.cpp
    Unit/test_mode_detector.cpp
    Unit/test_session_state.cpp
    Unit/test_buffer_pool.cpp
)
