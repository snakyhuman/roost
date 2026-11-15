# libghostty-vt from source (design.md §3, §11). Its CMake wrapper runs
# `zig build -Demit-lib-vt` and exports the imported targets ghostty-vt and
# ghostty-vt-static. roost links the static one.
#
# Pinned to the tag vt-1.0.1, commit 108cacb17ed8990840cd14abfce43d8ed8503ca1.
# To build against a local checkout instead:
#   cmake --preset dev -DFETCHCONTENT_SOURCE_DIR_GHOSTTY=/path/to/ghostty

set(ROOST_ZIG_MIN_VERSION 0.17.0)

# The wrapper would fail on a missing zig with a bare "could not find
# ZIG_EXECUTABLE"; say what to install instead.
find_program(ZIG_EXECUTABLE zig)
if(NOT ZIG_EXECUTABLE)
    message(FATAL_ERROR
        "zig is not in PATH. libghostty-vt is built with Zig ${ROOST_ZIG_MIN_VERSION} "
        "or newer: `brew install zig` on macOS, https://ziglang.org/download/ on Linux.")
endif()

execute_process(
    COMMAND "${ZIG_EXECUTABLE}" version
    OUTPUT_VARIABLE ROOST_ZIG_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(ROOST_ZIG_VERSION VERSION_LESS ROOST_ZIG_MIN_VERSION)
    message(FATAL_ERROR
        "zig ${ROOST_ZIG_VERSION} at ${ZIG_EXECUTABLE} is too old: "
        "libghostty-vt needs ${ROOST_ZIG_MIN_VERSION} or newer.")
endif()
message(STATUS "Found zig ${ROOST_ZIG_VERSION}: ${ZIG_EXECUTABLE}")

include(FetchContent)
FetchContent_Declare(ghostty
    GIT_REPOSITORY https://github.com/ghostty-org/ghostty.git
    GIT_TAG vt-1.0.1
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(ghostty)
