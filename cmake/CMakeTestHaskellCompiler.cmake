# CMakeTestHaskellCompiler.cmake
#
# Verifies that the Haskell compiler actually works by compiling a tiny
# source file.  If CMAKE_Haskell_COMPILER_FORCED is set (e.g. via a
# toolchain file or by CMakeDetermineHaskellCompiler.cmake), the test
# is skipped and the compiler is assumed to be working.

if(CMAKE_Haskell_COMPILER_FORCED)
  set(CMAKE_Haskell_COMPILER_WORKS TRUE)
  return()
endif()

include(CMakeTestCompilerCommon)

# Compile as a static library so we don't need a main entry point.
set(__CMAKE_SAVED_TRY_COMPILE_TARGET_TYPE ${CMAKE_TRY_COMPILE_TARGET_TYPE})
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

print_test_compiler_status("Haskell" "")

try_compile(CMAKE_Haskell_COMPILER_WORKS
  SOURCE_FROM_CONTENT smoke.hs
    "module Smoke where\nf :: Int -> Int\nf x = x + 1\n"
  OUTPUT_VARIABLE __CMAKE_Haskell_COMPILER_OUTPUT)

set(CMAKE_TRY_COMPILE_TARGET_TYPE ${__CMAKE_SAVED_TRY_COMPILE_TARGET_TYPE})

if(NOT CMAKE_Haskell_COMPILER_WORKS)
  print_test_compiler_result(STATUS "broken")
  file(APPEND "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeError.log"
    "Haskell compiler test failed with output:\n"
    "${__CMAKE_Haskell_COMPILER_OUTPUT}\n\n")
  message(FATAL_ERROR
    "The Haskell compiler '${CMAKE_Haskell_COMPILER}' is not able to compile "
    "a simple test program.\nVerify your installation of qdhc / hc.\n"
    "See CMakeFiles/CMakeError.log for details.")
else()
  print_test_compiler_result(STATUS "works")
  file(APPEND "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeOutput.log"
    "Haskell compiler test succeeded.\n\n")
endif()

unset(__CMAKE_Haskell_COMPILER_OUTPUT)
