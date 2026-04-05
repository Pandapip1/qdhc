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
  libllvm,
  tree-sitter,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "qdhc";
  version = "0-unstable";

  src = lib.cleanSource ../.;

  strictDeps = true;
  nativeBuildInputs = [
    libllvm
  ];
  buildInputs = [
    libllvm
    tree-sitter 
  ];

  doCheck = true;
  checkTarget = "test";

  installPhase = ''
    runHook preInstall
    install -Dm755 qdhc "$out/bin/qdhc"
    runHook postInstall
  '';

  meta = {
    description = "A compiler for a Haskell subset using tree-sitter + LLVM";
    longDescription = ''
      Compiles a subset of Haskell (integers, arithmetic, recursion,
      if/then/else, let/in, multi-arg functions) to LLVM IR, which can then
      be JIT-executed with llc or compiled to a native binary with llc + cc.
    '';
    license = lib.licenses.mit;
    platforms = lib.platforms.all;
    mainProgram = "qdhc";
  };
})
