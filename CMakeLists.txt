# Cross-platform build for the shared core and the CLI (macOS impl guide,
# "Build System and CI"). The Windows GUI keeps building via cpp/build.ps1;
# this covers the platform-neutral pieces (and, later, the macOS frontend).
#
#   cmake -B build-cmake && cmake --build build-cmake
#   ./build-cmake/fmdv-cli parse test.md
cmake_minimum_required(VERSION 3.16)
project(FMDV LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Short commit hash stamped into benchmark rows (empty if not a git checkout).
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE FMDV_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)

# --- shared platform-neutral core ---
add_library(fmdv_core STATIC
  core/str.cpp
  core/markdown.cpp
  core/edit_assist.cpp
  core/release_info.cpp)
target_include_directories(fmdv_core PUBLIC core)
if(MSVC)
  target_compile_options(fmdv_core PRIVATE /W4)
else()
  target_compile_options(fmdv_core PRIVATE -Wall -Wextra)
endif()

# --- CLI frontend ---
add_executable(fmdv-cli frontends/cli/fmdv_cli.cpp)
target_link_libraries(fmdv-cli PRIVATE fmdv_core)
target_compile_definitions(fmdv-cli PRIVATE
  FMDV_COMMIT="${FMDV_COMMIT}"
  FMDV_BUILD="cmake")

# The macOS AppKit frontend (frontends/macos) is added here once it exists.
