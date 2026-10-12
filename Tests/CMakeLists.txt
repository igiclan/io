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
    Unit/test_logging.cpp
    Unit/test_metrics.cpp
    Unit/test_config.cpp
    Unit/test_crypto.cpp
)
pf_add_test(pf_network_tests network
    Unit/main.cpp
    Network/test_connection.cpp
)

set(PF_TEST_LIBS pf_server_core pf_client_core)
pf_add_test(pf_integration_tests integration
    Unit/main.cpp
    Integration/test_end_to_end.cpp
    Integration/test_persistence.cpp
    Integration/test_rest_api.cpp
)
pf_add_test(pf_client_unit_tests unit
    Unit/main.cpp
    Unit/test_vhci_records.cpp
)
target_include_directories(pf_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
