# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.21)
project(coretrace_concurrency_analyzer VERSION 0.2.0)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(CheckLLVMVersion OPTIONAL)

if(COMMAND check_llvm_version)
    set(LLVM_MIN_REQUIRED_VERSION "20" CACHE STRING "Minimum required LLVM version")
    check_llvm_version(${LLVM_MIN_REQUIRED_VERSION})
else()
    message(WARNING "check_llvm_version() not available, skipping LLVM version check")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LLVM_LINK_LLVM_DYLIB ON CACHE BOOL "Prefer linking against monolithic LLVM target")

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include(${CMAKE_CURRENT_LIST_DIR}/cmake/compiler/coretrace-compiler.cmake)

option(BUILD_CLI "Build coretrace_concurrency_analyzer CLI tool" ON)
option(BUILD_SHARED_LIB "Build shared library variant" OFF)

set(CONCURRENCY_ANALYZER_SOURCES
    src/coretrace_concurrency_analysis.cpp
    src/coretrace_concurrency_error.cpp
    src/internal/analysis/concurrency_symbol_classifier.cpp
    src/internal/analysis/condition_wait_checker.cpp
    src/internal/analysis/condition_wait_collector.cpp
    src/internal/analysis/cross_tu/inter_tu_coordinator.cpp
    src/internal/analysis/cross_tu/program_symbol_index.cpp
    src/internal/analysis/data_race_checker.cpp
    src/internal/analysis/interprocedural_bindings.cpp
    src/internal/analysis/llvm_function_analysis_provider.cpp
    src/internal/analysis/lock_order_analyzer.cpp
    src/internal/analysis/lock_order_collector.cpp
    src/internal/analysis/lock_state_propagator.cpp
    src/internal/analysis/missing_join_detector.cpp
    src/internal/analysis/report_builder.cpp
    src/internal/diagnostics/compiler_diagnostic_parser.cpp
    src/internal/diagnostics/diagnostic_catalog.cpp
    src/internal/analysis/ir_utils.cpp
    src/internal/analysis/lock_effect_application.cpp
    src/internal/analysis/lock_wrapper_summaries.cpp
    src/internal/analysis/lock_scope_tracker.cpp
    src/internal/analysis/synchronization_effects.cpp
    src/internal/analysis/task_concurrency_analyzer.cpp
    src/internal/reporting/report_renderer.cpp
    src/internal/analysis/process_lifecycle_checker.cpp
    src/internal/analysis/process_lifecycle_collector.cpp
    src/internal/analysis/shared_access_collector.cpp
    src/internal/analysis/shared_object_binding_collector.cpp
    src/internal/analysis/signal_handler_collector.cpp
    src/internal/analysis/thread_argument_escape_collector.cpp
    src/internal/analysis/thread_context_propagator.cpp
    src/internal/analysis/thread_lifecycle_collector.cpp
    src/internal/analysis/thread_spawn_detector.cpp
    src/internal/analysis/tu_facts_builder.cpp
    src/internal/compilation_database.cpp
    src/internal/ir_cache.cpp
    src/internal/compile_command_builder.cpp
    src/internal/compilation_backend.cpp
    src/internal/ir_loader.cpp
    src/internal/temporary_bitcode_file.cpp
    src/coretrace_concurrency_analyzer.cpp
)

add_definitions(${LLVM_DEFINITIONS})

if(BUILD_SHARED_LIB)
    set(_coretrace_concurrency_lib_type SHARED)
else()
    set(_coretrace_concurrency_lib_type STATIC)
endif()

add_library(coretrace_concurrency_analyzer_lib ${_coretrace_concurrency_lib_type}
    ${CONCURRENCY_ANALYZER_SOURCES}
)

target_include_directories(coretrace_concurrency_analyzer_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(coretrace_concurrency_analyzer_lib SYSTEM
    PUBLIC
        ${LLVM_INCLUDE_DIRS}
)

# Treat external dependency headers as system includes to reduce warning noise.
target_include_directories(coretrace_concurrency_analyzer_lib SYSTEM
    PUBLIC
        $<TARGET_PROPERTY:cc::compilerlib_static,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(coretrace_concurrency_analyzer_lib
    PUBLIC
        cc::compilerlib_static
)

if(LLVM_LINK_LLVM_DYLIB AND TARGET LLVM)
    target_link_libraries(coretrace_concurrency_analyzer_lib PUBLIC LLVM)
else()
    llvm_map_components_to_libnames(coretrace_concurrency_llvm_libs
      analysis
      core
      irreader
      support
      bitreader
    )
    target_link_libraries(coretrace_concurrency_analyzer_lib
        PUBLIC
            ${coretrace_concurrency_llvm_libs}
    )
endif()

add_library(coretrace::coretrace_concurrency_analyzer_lib ALIAS coretrace_concurrency_analyzer_lib)

if(BUILD_CLI)
    add_executable(coretrace_concurrency_analyzer
      main.cpp
    )

    target_include_directories(coretrace_concurrency_analyzer
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(coretrace_concurrency_analyzer
      PRIVATE
        coretrace_concurrency_analyzer_lib
    )

    # --version answers from the same string the release tag is cut from, so a
    # binary someone has can be traced back to a tag without guessing.
    target_compile_definitions(coretrace_concurrency_analyzer
      PRIVATE
        CORETRACE_CONCURRENCY_ANALYZER_VERSION="${PROJECT_VERSION}"
    )

    # A consumer that FetchContents this project wants the library, not our CLI
    # landing in its install tree; only a standalone build installs.
    if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
        install(TARGETS coretrace_concurrency_analyzer RUNTIME DESTINATION bin)
    endif()
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  include(CTest)
  if(BUILD_TESTING)
    set(CORETRACE_TEST_COMMON_DEFINITIONS
      CORETRACE_PROJECT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
      CORETRACE_PROJECT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(coretrace_concurrency_arch_tests
      tests/unit/test_architecture.cpp
    )

    target_include_directories(coretrace_concurrency_arch_tests
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_definitions(coretrace_concurrency_arch_tests
      PRIVATE
        ${CORETRACE_TEST_COMMON_DEFINITIONS}
    )

    target_link_libraries(coretrace_concurrency_arch_tests
      PRIVATE
        coretrace_concurrency_analyzer_lib
    )

    add_test(NAME coretrace_concurrency_arch_tests COMMAND coretrace_concurrency_arch_tests)

    add_executable(coretrace_concurrency_compdb_tests
      tests/unit/test_compilation_database.cpp
    )

    target_include_directories(coretrace_concurrency_compdb_tests
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_definitions(coretrace_concurrency_compdb_tests
      PRIVATE
        ${CORETRACE_TEST_COMMON_DEFINITIONS}
    )

    target_link_libraries(coretrace_concurrency_compdb_tests
      PRIVATE
        coretrace_concurrency_analyzer_lib
    )

    add_test(NAME coretrace_concurrency_compdb_tests COMMAND coretrace_concurrency_compdb_tests)

    add_executable(coretrace_concurrency_ir_cache_tests
      tests/unit/test_ir_cache.cpp
    )

    target_include_directories(coretrace_concurrency_ir_cache_tests
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_definitions(coretrace_concurrency_ir_cache_tests
      PRIVATE
        ${CORETRACE_TEST_COMMON_DEFINITIONS}
    )

    target_link_libraries(coretrace_concurrency_ir_cache_tests
      PRIVATE
        coretrace_concurrency_analyzer_lib
    )

    add_test(NAME coretrace_concurrency_ir_cache_tests COMMAND coretrace_concurrency_ir_cache_tests)

    add_executable(coretrace_concurrency_error_tests
      tests/unit/test_error_model.cpp
    )

    target_compile_definitions(coretrace_concurrency_error_tests
      PRIVATE
        ${CORETRACE_TEST_COMMON_DEFINITIONS}
    )

    target_link_libraries(coretrace_concurrency_error_tests
      PRIVATE
        coretrace_concurrency_analyzer_lib
    )

    add_test(NAME coretrace_concurrency_error_tests COMMAND coretrace_concurrency_error_tests)

    add_executable(coretrace_concurrency_analysis_tests
      tests/unit/test_concurrency_analysis.cpp
    )

    target_compile_definitions(coretrace_concurrency_analysis_tests
      PRIVATE
        ${CORETRACE_TEST_COMMON_DEFINITIONS}
    )

    target_link_libraries(coretrace_concurrency_analysis_tests
      PRIVATE
        coretrace_concurrency_analyzer_lib
    )

    add_test(NAME coretrace_concurrency_analysis_tests COMMAND coretrace_concurrency_analysis_tests)

    add_executable(coretrace_concurrency_cross_tu_tests
      tests/unit/test_cross_tu_analysis.cpp
    )

    target_compile_definitions(coretrace_concurrency_cross_tu_tests
      PRIVATE
        ${CORETRACE_TEST_COMMON_DEFINITIONS}
    )

    target_link_libraries(coretrace_concurrency_cross_tu_tests
      PRIVATE
        coretrace_concurrency_analyzer_lib
    )

    add_test(NAME coretrace_concurrency_cross_tu_tests COMMAND coretrace_concurrency_cross_tu_tests)

    add_executable(coretrace_concurrency_cli_cpp_tests
      tests/integration/cli/test_cli_cpp.cpp
    )

    target_compile_definitions(coretrace_concurrency_cli_cpp_tests
      PRIVATE
        ${CORETRACE_TEST_COMMON_DEFINITIONS}
    )

    add_dependencies(coretrace_concurrency_cli_cpp_tests coretrace_concurrency_analyzer)
    add_test(NAME coretrace_concurrency_cli_cpp_tests COMMAND coretrace_concurrency_cli_cpp_tests)

    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import ctestfw"
        RESULT_VARIABLE CORETRACE_CTESTFW_IMPORT_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
      )

      if(CORETRACE_CTESTFW_IMPORT_RESULT EQUAL 0)
        add_test(
          NAME coretrace_concurrency_cli_integration_tests
          COMMAND "${Python3_EXECUTABLE}"
                  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli/test_analyzer.py"
        )
      else()
        message(STATUS "Skipping cli integration tests: ctestfw module not available")
      endif()

      execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import pytest"
        RESULT_VARIABLE CORETRACE_PYTEST_IMPORT_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
      )

      if(CORETRACE_PYTEST_IMPORT_RESULT EQUAL 0)
        add_test(
          NAME coretrace_concurrency_human_output_golden_tests
          COMMAND "${Python3_EXECUTABLE}" -m pytest
                  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli/test_human_output_golden.py"
                  -q
        )
      else()
        message(STATUS "Skipping human output golden tests: pytest module not available")
      endif()
    else()
      message(STATUS "Skipping cli integration tests: Python3 interpreter not found")
    endif()
  endif()
endif()

# Only add helper targets when building standalone to avoid collisions.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  add_custom_target(format
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Run clang-format on source files")

  add_custom_target(format-check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format-check.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Verify clang-format compliance")
endif()
