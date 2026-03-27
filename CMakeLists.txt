cmake_minimum_required(VERSION 3.21)
project(coretrace_concurrency_analyzer)

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
    src/coretrace_concurrency_error.cpp
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

    target_link_libraries(coretrace_concurrency_analyzer
      PRIVATE
        coretrace_concurrency_analyzer_lib
    )
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
