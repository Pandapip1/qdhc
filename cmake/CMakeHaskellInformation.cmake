# CMakeHaskellInformation.cmake
#
# Defines build rules for the Haskell language:
#   CMAKE_Haskell_COMPILE_OBJECT   — .hs -> .o
#   CMAKE_Haskell_CREATE_*         — linking / archiving

include(CMakeLanguageInformation)

if(UNIX)
  set(CMAKE_Haskell_OUTPUT_EXTENSION .o)
else()
  set(CMAKE_Haskell_OUTPUT_EXTENSION .obj)
endif()

# Per-configuration flags initialised from the environment variable HFLAGS.
set(CMAKE_Haskell_FLAGS_INIT "$ENV{HFLAGS} ${CMAKE_Haskell_FLAGS_INIT}")
cmake_initialize_per_config_variable(CMAKE_Haskell_FLAGS
  "Flags passed to the Haskell compiler for all build types")

# ── Compile rule ────────────────────────────────────────────────────────────
# hc is a GCC-compatible driver: -c produces a native object file.
# <DEFINES> and <INCLUDES> are passed so that cmake targets which mix Haskell
# with C can propagate include paths; qdhc silently ignores them.
if(NOT CMAKE_Haskell_COMPILE_OBJECT)
  set(CMAKE_Haskell_COMPILE_OBJECT
    "<CMAKE_Haskell_COMPILER> <FLAGS> -c -o <OBJECT> <SOURCE>")
endif()

# ── Archive (static library) rules ─────────────────────────────────────────
if(NOT DEFINED CMAKE_Haskell_ARCHIVE_CREATE)
  set(CMAKE_Haskell_ARCHIVE_CREATE  "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
endif()
if(NOT DEFINED CMAKE_Haskell_ARCHIVE_APPEND)
  set(CMAKE_Haskell_ARCHIVE_APPEND  "<CMAKE_AR> q  <TARGET> <LINK_FLAGS> <OBJECTS>")
endif()
if(NOT DEFINED CMAKE_Haskell_ARCHIVE_FINISH)
  set(CMAKE_Haskell_ARCHIVE_FINISH  "<CMAKE_RANLIB> <TARGET>")
endif()

# ── Shared library rule ─────────────────────────────────────────────────────
# hc delegates linking to the system cc, so the shared-library flags from the
# platform file apply unchanged.
if(NOT CMAKE_Haskell_CREATE_SHARED_LIBRARY)
  set(CMAKE_Haskell_CREATE_SHARED_LIBRARY
    "<CMAKE_Haskell_COMPILER> <CMAKE_SHARED_LIBRARY_Haskell_FLAGS> <LINK_FLAGS> \
<CMAKE_SHARED_LIBRARY_CREATE_Haskell_FLAGS> \
<SONAME_FLAG><TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>")
endif()

if(NOT CMAKE_Haskell_CREATE_SHARED_MODULE)
  set(CMAKE_Haskell_CREATE_SHARED_MODULE ${CMAKE_Haskell_CREATE_SHARED_LIBRARY})
endif()

# ── Executable link rule ────────────────────────────────────────────────────
# hc with no -c flag: compile-and-link.  For link-only (all inputs are .o),
# hc detects no .hs source and delegates to cc automatically.
if(NOT CMAKE_Haskell_LINK_EXECUTABLE)
  set(CMAKE_Haskell_LINK_EXECUTABLE
    "<CMAKE_Haskell_COMPILER> <FLAGS> <CMAKE_Haskell_LINK_FLAGS> <LINK_FLAGS> \
<OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
endif()

set(CMAKE_Haskell_INFORMATION_LOADED 1)
