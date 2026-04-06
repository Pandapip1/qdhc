cmake_minimum_required(VERSION 3.20)
project(qdhc VERSION 0.1.0 LANGUAGES C CXX)

# ── Dependencies ────────────────────────────────────────────────────────────

# LLVM — prefer an installed LLVMConfig.cmake (provided by llvm-dev / libllvm).
# Falls back gracefully when only llvm-config is available via the legacy path.
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

# Ask llvm-config for the exact library flags needed to link everything,
# including all targets (required for LLVMInitializeAllTargets() etc.).
# llvm_map_components_to_libnames is unreliable for sub-component names that
# vary between LLVM versions; llvm-config --libs always returns the right set.
find_program(LLVM_CONFIG_EXECUTABLE
  NAMES llvm-config "llvm-config-${LLVM_PACKAGE_VERSION}" llvm-config-21
  HINTS "${LLVM_TOOLS_BINARY_DIR}"
  REQUIRED)

execute_process(
  COMMAND "${LLVM_CONFIG_EXECUTABLE}" --libs
          core analysis bitwriter passes mcjit executionengine native
          codegen object all-targets
  OUTPUT_VARIABLE _llvm_config_libs
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)

# llvm-config returns space-separated -lFoo flags; convert to a cmake list
separate_arguments(LLVM_LIBS UNIX_COMMAND "${_llvm_config_libs}")

execute_process(
  COMMAND "${LLVM_CONFIG_EXECUTABLE}" --ldflags
  OUTPUT_VARIABLE _llvm_config_ldflags
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)

separate_arguments(_llvm_ldflags UNIX_COMMAND "${_llvm_config_ldflags}")

# tree-sitter — try pkg-config first, then fall back to a plain search.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(TREE_SITTER IMPORTED_TARGET tree-sitter)
endif()

if(NOT TREE_SITTER_FOUND)
  # Manual fallback: search standard paths + any prefix passed via CMAKE_PREFIX_PATH
  find_library(TREE_SITTER_LIB NAMES tree-sitter REQUIRED)
  find_path(TREE_SITTER_INCLUDE tree_sitter/api.h REQUIRED)
  add_library(TreeSitter::TreeSitter UNKNOWN IMPORTED)
  set_target_properties(TreeSitter::TreeSitter PROPERTIES
    IMPORTED_LOCATION "${TREE_SITTER_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${TREE_SITTER_INCLUDE}")
  set(TREE_SITTER_TARGET TreeSitter::TreeSitter)
else()
  set(TREE_SITTER_TARGET PkgConfig::TREE_SITTER)
endif()

# ── Bundled tree-sitter grammar (grammar/src/) ───────────────────────────────
# These are the pre-generated C sources for tree-sitter-haskell, shipped in the
# repo so no tree-sitter CLI or grammar package is needed at build time.

add_library(ts_haskell STATIC
  grammar/src/parser.c
  grammar/src/scanner.c
)
target_include_directories(ts_haskell PRIVATE grammar/src)
# Suppress warnings from generated grammar code
target_compile_options(ts_haskell PRIVATE -w)

# ── qdhc executable ─────────────────────────────────────────────────────────

add_executable(qdhc src/compiler.c)

target_include_directories(qdhc PRIVATE
  grammar/src
  ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(qdhc PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_options(qdhc PRIVATE
  -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(qdhc PRIVATE
  ts_haskell
  ${TREE_SITTER_TARGET}
  ${LLVM_LIBS}
  ${_llvm_ldflags}
  m
)

# LLVM's static libraries are C++; even though qdhc's own source is C, the
# linker must be g++ (or clang++) so libstdc++ is included automatically.
set_target_properties(qdhc PROPERTIES LINKER_LANGUAGE CXX)

# ── Install ─────────────────────────────────────────────────────────────────

set(QDHC_CMAKE_DIR "${CMAKE_INSTALL_PREFIX}/share/qdhc/cmake"
    CACHE PATH "Install directory for qdhc's CMake language modules")

install(TARGETS qdhc
  RUNTIME DESTINATION bin
)

# hc symlink — created as a post-install script so it works with DESTDIR too
install(CODE "
  set(_bindir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin\")
  execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink qdhc \"\${_bindir}/hc\")
")

install(FILES
  cmake/CMakeDetermineHaskellCompiler.cmake
  cmake/CMakeHaskellCompiler.cmake.in
  cmake/CMakeHaskellInformation.cmake
  cmake/CMakeTestHaskellCompiler.cmake
  DESTINATION "${QDHC_CMAKE_DIR}"
)

# ── Tests ────────────────────────────────────────────────────────────────────

enable_testing()

# Each test is a self-contained .hs file in tests/ that exits 0 on pass, 1 on
# fail.  To add a test, just drop a new .hs file into tests/.

file(GLOB _test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hs")

foreach(_hs_file ${_test_sources})
  get_filename_component(_name "${_hs_file}" NAME_WE)
  add_test(NAME "qdhc_${_name}" COMMAND qdhc --run "${_hs_file}")
endforeach()
