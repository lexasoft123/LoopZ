cmake_minimum_required(VERSION 3.24)
# Initialize before project(): Darwin platform detection otherwise caches an
# empty deployment target and silently builds for the SDK's host OS version.
if((CMAKE_HOST_APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Darwin") AND
   (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL ""))
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.2" CACHE STRING "Minimum macOS" FORCE)
endif()
project(LoopZ VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
option(LOOPZ_BUILD_MACOS_BACKEND "Build the macOS driver, capture and helper" ${APPLE})
option(LOOPZ_BUILD_BENCHMARKS "Build offline routing benchmarks" OFF)
if(LOOPZ_BUILD_MACOS_BACKEND)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "The macOS backend requires an Apple target; use -DLOOPZ_BUILD_MACOS_BACKEND=OFF for the portable core")
  endif()
  enable_language(OBJCXX)
endif()
include(CTest)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(loopz_strict_target target)
  if(MSVC)
    # Realtime timeline structures deliberately contain alignment padding.
    # Keep their layout and all other warnings strict on MSVC.
    target_compile_options(${target} PRIVATE /W4 /WX /EHsc /wd4324)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()
function(loopz_test_target target)
  loopz_strict_target(${target})
  if(MSVC)
    target_compile_options(${target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
endfunction()

add_custom_target(format
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/format_code.py"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  COMMENT "Formatting LoopZ sources")
add_custom_target(format-check
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/format_code.py" --check
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  COMMENT "Checking LoopZ source formatting")
add_subdirectory(cmake/singz)
add_subdirectory(native/core)
if(LOOPZ_BUILD_MACOS_BACKEND)
  add_subdirectory(native/platform/macos)
  add_subdirectory(native/platform/macos/editor)
  add_subdirectory(native/platform/macos/desktop)
endif()
if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(tests/editor)
endif()
