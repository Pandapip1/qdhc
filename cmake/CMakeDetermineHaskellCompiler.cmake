# CMakeDetermineHaskellCompiler.cmake
#
# Determines the Haskell compiler (hc) to use.
# Loaded automatically by CMake when a project enables the Haskell language:
#
#   list(APPEND CMAKE_MODULE_PATH "<prefix>/share/qdhc/cmake")
#   project(myproject LANGUAGES Haskell)
#
# Honours (in priority order):
#   1. CMAKE_Haskell_COMPILER set before project()
#   2. Environment variable HC
#   3. The name "hc" searched on PATH

if(NOT CMAKE_Haskell_COMPILER)
  # 1. Environment variable HC
  if(NOT "$ENV{HC}" STREQUAL "")
    get_filename_component(CMAKE_Haskell_COMPILER_INIT "$ENV{HC}"
      PROGRAM PROGRAM_ARGS CMAKE_Haskell_FLAGS_ENV_INIT)
    if(CMAKE_Haskell_FLAGS_ENV_INIT)
      set(CMAKE_Haskell_COMPILER_ARG1 "${CMAKE_Haskell_FLAGS_ENV_INIT}"
          CACHE STRING "Arguments to the Haskell compiler")
    endif()
    if(NOT EXISTS "${CMAKE_Haskell_COMPILER_INIT}")
      message(FATAL_ERROR
        "HC environment variable set to '${CMAKE_Haskell_COMPILER_INIT}' "
        "but that file does not exist.")
    endif()
  endif()

  # 2. Search PATH for hc
  if(NOT CMAKE_Haskell_COMPILER_INIT)
    set(CMAKE_Haskell_COMPILER_LIST hc ghc)
  endif()

  _cmake_find_compiler(Haskell)
else()
  _cmake_find_compiler_path(Haskell)
endif()

mark_as_advanced(CMAKE_Haskell_COMPILER)

# Skip the generic compiler-ID probe — qdhc/hc has no preprocessor and
# cannot compile the CMake-generated C-style ID source.  We declare the
# ID directly here.
set(CMAKE_Haskell_COMPILER_ID       "qdhc")
set(CMAKE_Haskell_COMPILER_ID_RUN   1)

# Write the persistent compiler-info file into the build tree.
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/CMakeHaskellCompiler.cmake.in"
  "${CMAKE_PLATFORM_INFO_DIR}/CMakeHaskellCompiler.cmake"
  @ONLY)

set(CMAKE_Haskell_COMPILER_ENV_VAR "HC")
