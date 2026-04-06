# haskell-compiler — Makefile

LLVM_CFG = llvm-config

LLVM_CFLAGS  := $(shell $(LLVM_CFG) --cflags)
LLVM_LDFLAGS := $(shell $(LLVM_CFG) --ldflags)
LLVM_LIBS    := $(shell $(LLVM_CFG) --libs core analysis bitwriter passes mcjit executionengine native codegen object)

TS_DIR = grammar/src

CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter \
         -I$(TS_DIR) \
         $(LLVM_CFLAGS)

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
CMAKEDIR    ?= $(PREFIX)/share/qdhc/cmake

.PHONY: all clean test install uninstall

all: qdhc

ts_parser.o: $(TS_DIR)/parser.c
	$(CC) -O2 -c -o $@ $< -I$(TS_DIR)

ts_scanner.o: $(TS_DIR)/scanner.c
	$(CC) -O2 -c -o $@ $< -I$(TS_DIR)

compiler.o: src/compiler.c
	$(CC) $(CFLAGS) -c -o $@ $<

qdhc: compiler.o ts_parser.o ts_scanner.o
	$(CC) -o $@ $^ $(LLVM_LDFLAGS) $(LLVM_LIBS) -ltree-sitter -lm

# ── test suite ─────────────────────────────────────────────────────────────────
define RUN_TEST =
	@{ \
	  printf '  %-38s' "$(1):"; \
	  mkdir -p _testout/$(1); \
	  printf '$(2)' > _testout/$(1)/input.hs; \
	  got=$$(./qdhc --run --dump-dir _testout/$(1) _testout/$(1)/input.hs \
	          2>_testout/$(1)/stderr.txt); \
	  if [ "$$got" = "$(3)" ]; then \
	    echo "ok"; \
	  else \
	    echo "FAIL  expected=$(3)  got=$$got"; \
	    if [ -n "$(VERBOSE)" ]; then \
	      echo "--- input ---" >&2; \
	      cat _testout/$(1)/input.hs >&2; \
	      echo "--- stderr ---" >&2; \
	      cat _testout/$(1)/stderr.txt >&2; \
	      echo "--- cst ---" >&2; \
	      cat _testout/$(1)/cst.txt >&2; \
	      echo "--- ir ---" >&2; \
	      cat _testout/$(1)/ir.ll >&2; \
	    fi; \
	    exit 1; \
	  fi; \
	}
endef

test: qdhc
	@echo "=== qdhc test suite ==="
	$(call RUN_TEST,addition,\
module M where\nmain = let r = 3 + 4 in r,7)
	$(call RUN_TEST,subtraction,\
module M where\nmain = let r = 10 - 3 in r,7)
	$(call RUN_TEST,multiplication,\
module M where\nmain = let r = 3 * 7 in r,21)
	$(call RUN_TEST,div,\
module M where\nmain = let r = 42 `div` 6 in r,7)
	$(call RUN_TEST,mod,\
module M where\nmain = let r = 17 `mod` 5 in r,2)
	$(call RUN_TEST,negation,\
module M where\nmain = let r = (-7) in r,-7)
	$(call RUN_TEST,eq-true,\
module M where\nmain = let r = if 3 == 3 then 1 else 0 in r,1)
	$(call RUN_TEST,eq-false,\
module M where\nmain = let r = if 3 == 4 then 1 else 0 in r,0)
	$(call RUN_TEST,neq,\
module M where\nmain = let r = if 3 /= 4 then 1 else 0 in r,1)
	$(call RUN_TEST,lt,\
module M where\nmain = let r = if 2 < 5 then 1 else 0 in r,1)
	$(call RUN_TEST,gte,\
module M where\nmain = let r = if 9 >= 9 then 1 else 0 in r,1)
	$(call RUN_TEST,double,\
module M where\ndouble x = x + x\nmain = let r = double 21 in r,42)
	$(call RUN_TEST,sub-in-body,\
module M where\nf n = n - 1\nmain = let r = f 8 in r,7)
	$(call RUN_TEST,sub-in-parens,\
module M where\nf n = n * (n - 1)\nmain = let r = f 5 in r,20)
	$(call RUN_TEST,factorial-5,\
module M where\nf n = if n == 0 then 1 else n * f (n - 1)\nmain = let r = f 5 in r,120)
	$(call RUN_TEST,factorial-10,\
module M where\nf n = if n == 0 then 1 else n * f (n - 1)\nmain = let r = f 10 in r,3628800)
	$(call RUN_TEST,fib-10,\
module M where\nfib n = if n <= 1 then n else fib (n-1) + fib (n-2)\nmain = let r = fib 10 in r,55)
	$(call RUN_TEST,add-2arg,\
module M where\nadd x y = x + y\nmain = let r = add 19 23 in r,42)
	$(call RUN_TEST,mul3-3arg,\
module M where\nmul3 x y z = x * y * z\nmain = let r = mul3 2 3 7 in r,42)
	$(call RUN_TEST,sign-pos,\
module M where\ns n = if n > 0 then 1 else if n < 0 then -1 else 0\nmain = let r = s 99 in r,1)
	$(call RUN_TEST,sign-neg,\
module M where\ns n = if n > 0 then 1 else if n < 0 then -1 else 0\nmain = let r = s (-5) in r,-1)
	$(call RUN_TEST,sign-zero,\
module M where\ns n = if n > 0 then 1 else if n < 0 then -1 else 0\nmain = let r = s 0 in r,0)
	$(call RUN_TEST,let-multi,\
module M where\nmain = let { a = 6; b = 7 } in a * b,42)
	$(call RUN_TEST,let-dep,\
module M where\nmain = let { x = 10; y = x + 32 } in y,42)
	$(call RUN_TEST,comment-before-decl,\
module M where\n-- a comment\nadd x y = x + y\nmain = let r = add 19 23 in r,42)
	@echo "All 25 tests passed."

clean:
	rm -f *.o qdhc
	rm -rf _testout

install: qdhc
	install -Dm755 qdhc "$(DESTDIR)$(BINDIR)/qdhc"
	ln -sf qdhc         "$(DESTDIR)$(BINDIR)/hc"
	install -Dm644 cmake/CMakeDetermineHaskellCompiler.cmake \
	    "$(DESTDIR)$(CMAKEDIR)/CMakeDetermineHaskellCompiler.cmake"
	install -Dm644 cmake/CMakeHaskellCompiler.cmake.in \
	    "$(DESTDIR)$(CMAKEDIR)/CMakeHaskellCompiler.cmake.in"
	install -Dm644 cmake/CMakeHaskellInformation.cmake \
	    "$(DESTDIR)$(CMAKEDIR)/CMakeHaskellInformation.cmake"
	install -Dm644 cmake/CMakeTestHaskellCompiler.cmake \
	    "$(DESTDIR)$(CMAKEDIR)/CMakeTestHaskellCompiler.cmake"

uninstall:
	rm -f  "$(DESTDIR)$(BINDIR)/qdhc"
	rm -f  "$(DESTDIR)$(BINDIR)/hc"
	rm -rf "$(DESTDIR)$(CMAKEDIR)"
