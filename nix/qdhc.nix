# nix/qdhc.nix
#
# nixpkgs ships tree-sitter >= 0.21.0, which includes the upstream fix for
# ts_node_field_name_for_child (PR #2104). No patches required.
#
# The tree-sitter-haskell grammar sources are bundled in grammar/src/ so
# neither the tree-sitter CLI nor any grammar package is needed at build time.

{
  lib,
  stdenv,
  cmake,
  libllvm,
  tree-sitter,
  pkg-config,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "qdhc";
  version = "0-unstable";

  src = lib.cleanSource ../.;

  strictDeps = true;
  nativeBuildInputs = [
    cmake
    pkg-config
    libllvm
  ];
  buildInputs = [
    libllvm
    tree-sitter
  ];

  cmakeFlags = [
    (lib.cmakeFeature "QDHC_CMAKE_DIR" "${placeholder "out"}/share/qdhc/cmake")
  ];

  doCheck = true;

  meta = {
    description = "A compiler for a Haskell subset using tree-sitter + LLVM";
    license = lib.licenses.mit;
    platforms = lib.platforms.all;
    mainProgram = "qdhc";
  };
})
