cmake_minimum_required(VERSION 3.28...4.4)

project(
    ToN2026
    VERSION 1.0.0
    DESCRIPTION "Proactive Dual-Link Failure Restoration in Optical WDM Networks (ToN 2026)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(ton_compile_options INTERFACE)
target_compile_options(
    ton_compile_options
    INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        $<$<CONFIG:Release>:-O3;-march=native;-flto;-DNDEBUG>
        $<$<NOT:$<CONFIG:Release>>:-D_GLIBCXX_ASSERTIONS;-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>
)
target_link_options(
    ton_compile_options
    INTERFACE
        $<$<CONFIG:Release>:-flto>
)

if(ENABLE_SANITIZERS)
    target_compile_options(
        ton_compile_options
        INTERFACE
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
            -fno-omit-frame-pointer
            -g
            -O1
    )
    target_link_options(
        ton_compile_options
        INTERFACE
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
    )
endif()

add_library(
    ton_core
    STATIC
        src/core/graph.cpp
        src/core/scenarios.cpp
        src/core/path_pool.cpp
        src/orchestration/network_state.cpp
        src/orchestration/provisioning_harness.cpp
        src/orchestration/telemetry.cpp
        src/solvers/no_reroute_solver.cpp
        src/solvers/greedy_solver.cpp
        src/solvers/dsatur_solver.cpp
        src/verification/invariant_auditor.cpp
)
target_include_directories(ton_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ton_core PUBLIC ton_compile_options)

add_executable(ton_engine src/main.cpp)
target_link_libraries(ton_engine PRIVATE ton_core)

enable_testing()

add_library(
    catch2_amalgamated
    STATIC
        third_party/catch2/catch_amalgamated.cpp
)
target_include_directories(catch2_amalgamated SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/catch2")
target_compile_options(catch2_amalgamated PRIVATE -w)

add_executable(
    ton_tests
        tests/test_helpers.cpp
        tests/test_types_and_graph.cpp
        tests/test_scenarios_and_paths.cpp
        tests/test_state_and_telemetry.cpp
        tests/test_inner_solvers.cpp
        tests/test_sim_orchestrator.cpp
        tests/test_seq_orchestrator.cpp
        tests/test_phase3_gate.cpp
)
target_include_directories(ton_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(ton_tests PRIVATE ton_core catch2_amalgamated)
target_compile_definitions(ton_tests PRIVATE TON_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(
    ton_tests
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

add_test(NAME ton_tests COMMAND ton_tests)
