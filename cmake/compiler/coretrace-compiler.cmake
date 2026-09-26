# SPDX-License-Identifier: Apache-2.0
include(FetchContent)

# Optional ASAN enablement at the top-level to match the cc dependency.
option(ENABLE_DEBUG_ASAN "Enable debug symbols and AddressSanitizer" OFF)
if(DEFINED DEBUG_ASAN)
    set(ENABLE_DEBUG_ASAN ${DEBUG_ASAN} CACHE BOOL
        "Enable debug symbols and AddressSanitizer" FORCE)
endif()

# A tag of this project has to build the same way in a year as it does today,
# and GIT_TAG main would let the compiler backend drift underneath a fixed tag.
# The commit below is v0.9.0-2-g8ae9765, the tree this project is validated
# against; move it deliberately, as its own commit, not as a side effect.
FetchContent_Declare(
    cc
    GIT_REPOSITORY https://github.com/CoreTrace/coretrace-compiler.git
    GIT_TAG 8ae9765c133b8e39bb17fcfee302e9b41e57a940
)
FetchContent_MakeAvailable(cc)
