cmake_minimum_required(VERSION 3.28)
project(consumer_example CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Prefer the local parent checkout when this example is built from the
# repository tree; fallback to GitHub when used standalone.
set(CONCURRENCY_ANALYZER_LOCAL_SOURCE "${CMAKE_CURRENT_LIST_DIR}/..")
if(EXISTS "${CONCURRENCY_ANALYZER_LOCAL_SOURCE}/CMakeLists.txt")
  message(STATUS "Using local concurrency analyzer source: ${CONCURRENCY_ANALYZER_LOCAL_SOURCE}")
  FetchContent_Declare(
    concurrency_analyzer
    SOURCE_DIR "${CONCURRENCY_ANALYZER_LOCAL_SOURCE}"
  )
else()
  message(STATUS "Using remote concurrency analyzer source from GitHub")
  FetchContent_Declare(
    concurrency_analyzer
    GIT_REPOSITORY https://github.com/CoreTrace/coretrace-concurrency-analyzer.git
    GIT_TAG main
  )
endif()

FetchContent_MakeAvailable(concurrency_analyzer)

add_executable(concurrency_consumer src/main.cpp)

target_link_libraries(concurrency_consumer
    PRIVATE
        coretrace::coretrace_concurrency_analyzer_lib
)
