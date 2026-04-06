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

# Tests are defined as three parallel lists: names, sources, expected outputs.
#
# IMPORTANT: cmake's list() always splits on semicolons, even inside strings
# set with quotes.  The Haskell sources contain semicolons (e.g. let { a=1; b=2 })
# so we must NOT store them in a single flat list.  Parallel lists with one
# cmake variable per field sidestep this entirely.
#
# Newlines in sources are encoded as the two-character sequence \n and expanded
# with string(REPLACE) just before file(WRITE).

set(T_NAMES "")
set(T_SRCS  "")
set(T_EXPRS "")

macro(add_hs_test name src expected)
  list(APPEND T_NAMES "${name}")
  list(APPEND T_SRCS  "${src}")
  list(APPEND T_EXPRS "${expected}")
endmacro()

add_hs_test(addition           "module M where\nmain = let r = 3 + 4 in r"          7)
add_hs_test(subtraction        "module M where\nmain = let r = 10 - 3 in r"         7)
add_hs_test(multiplication     "module M where\nmain = let r = 3 * 7 in r"          21)
add_hs_test(div                "module M where\nmain = let r = 42 `div` 6 in r"     7)
add_hs_test(mod                "module M where\nmain = let r = 17 `mod` 5 in r"     2)
add_hs_test(negation           "module M where\nmain = let r = (-7) in r"           -7)
add_hs_test(eq-true            "module M where\nmain = let r = if 3 == 3 then 1 else 0 in r" 1)
add_hs_test(eq-false           "module M where\nmain = let r = if 3 == 4 then 1 else 0 in r" 0)
add_hs_test(neq                "module M where\nmain = let r = if 3 /= 4 then 1 else 0 in r" 1)
add_hs_test(lt                 "module M where\nmain = let r = if 2 < 5 then 1 else 0 in r"  1)
add_hs_test(gte                "module M where\nmain = let r = if 9 >= 9 then 1 else 0 in r" 1)
add_hs_test(double             "module M where\ndouble x = x + x\nmain = let r = double 21 in r" 42)
add_hs_test(sub-in-body        "module M where\nf n = n - 1\nmain = let r = f 8 in r"       7)
add_hs_test(sub-in-parens      "module M where\nf n = n * (n - 1)\nmain = let r = f 5 in r" 20)
add_hs_test(factorial-5        "module M where\nf n = if n == 0 then 1 else n * f (n - 1)\nmain = let r = f 5 in r"  120)
add_hs_test(factorial-10       "module M where\nf n = if n == 0 then 1 else n * f (n - 1)\nmain = let r = f 10 in r" 3628800)
add_hs_test(fib-10             "module M where\nfib n = if n <= 1 then n else fib (n-1) + fib (n-2)\nmain = let r = fib 10 in r" 55)
add_hs_test(add-2arg           "module M where\nadd x y = x + y\nmain = let r = add 19 23 in r"      42)
add_hs_test(mul3-3arg          "module M where\nmul3 x y z = x * y * z\nmain = let r = mul3 2 3 7 in r" 42)
add_hs_test(sign-pos           "module M where\ns n = if n > 0 then 1 else if n < 0 then -1 else 0\nmain = let r = s 99 in r"   1)
add_hs_test(sign-neg           "module M where\ns n = if n > 0 then 1 else if n < 0 then -1 else 0\nmain = let r = s (-5) in r" -1)
add_hs_test(sign-zero          "module M where\ns n = if n > 0 then 1 else if n < 0 then -1 else 0\nmain = let r = s 0 in r"    0)
add_hs_test(let-multi          "module M where\nmain = let { a = 6[SEMI] b = 7 } in a * b" 42)
add_hs_test(let-dep            "module M where\nmain = let { x = 10[SEMI] y = x + 32 } in y" 42)
add_hs_test(comment-before-decl "module M where\n-- a comment\nadd x y = x + y\nmain = let r = add 19 23 in r" 42)

list(LENGTH T_NAMES _ntest)
set(_i 0)
while(_i LESS _ntest)
  list(GET T_NAMES ${_i} _name)
  list(GET T_SRCS  ${_i} _src)
  list(GET T_EXPRS ${_i} _expected)
  math(EXPR _i "${_i} + 1")

  # Expand \n to real newlines, then restore semicolons from the [SEMI] placeholder
  string(REPLACE "\\n" "\n" _src "${_src}")
  string(REPLACE "[SEMI]" ";" _src "${_src}")

  set(_input_file "${CMAKE_CURRENT_BINARY_DIR}/testinput/${_name}.hs")
  file(WRITE "${_input_file}" "${_src}\n")

  add_test(NAME "qdhc_${_name}" COMMAND qdhc --run "${_input_file}")
  # ctest concatenates stdout+stderr and matches the full string; \n is a
  # literal newline in the regex, which correctly anchors the expected value.
  set_tests_properties("qdhc_${_name}" PROPERTIES
    PASS_REGULAR_EXPRESSION "\n${_expected}\n"
  )
endwhile()
