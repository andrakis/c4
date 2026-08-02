# C4KE and suite Makefile
# This file is ugly. There are likely much better ways to do this.
#
# Target 'all' builds:
#   c4, c4m, c4cc, c4rdump, c4rlink native executables.
#   Uses c4cc to build the various .c4r files.
#
# Make targets:
#   pkg           Produce a package of the source tree to PKG (default: package.tgz)
#   run           Run C4KE under c4m (fastest)
#   run-c4        Run C4KE under c4, running under c4m (slowest)
#   run-alt       Alternate invocations for c4m, fixing a timing issue on some systems
#   run-c4-alt    ..
#   run-vg        Run C4KE under valgrind
#   run-alt-vg
#   run-c4-vg
#   run-c4-alt-vg
#   test          Test C4KE using the 'innerbench' tool
#   test-alt
#   test-c4
#   test-c4-alt
#   test-massive       Test C4KE using 'innerbench' and a large number of
#                      child processes.
#   test-massive-alt
#   test-massive-c4
#   test-massive-c4-alt
#
# What is u0?
# u0 is the C4KE user runtime, and is provides an interface to C4KE as well as
# a standard library.

PKG       := package.tgz
NATIVE_CC := gcc
EXTRA_CC  :=
NATIVE_CC_OPTS := -O2 -g -idirafter include -I . $(EXTRA_CC)
NATIVE_TARGETS := c4 c4m c4cc
# Preprocessor, will use our own at some point
# We use our own include directories, and have some stdlib style headers.
# We also define the following symbols, which TODO needd to be narrowed down to a single
# definition instead of the 4 we have.
PREPROC   := gcc -E -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -C
C4        := ./c4
C4M       := ./c4m
C4CC      := ./c4cc
C4RDUMP   := ./c4rdump
C4RLINK   := ./c4rlink
SRCS      := src
INCLUDE   := include
C4CC_SRCS := $(SRCS)/c4cc/c4cc.c $(SRCS)/c4cc/asm-c4r.c
# Version of C4CC compiled to .c4r format
C4R_C4CC_SRCS := $(U0) load-c4r.c $(SRCS)/c4cc/c4cc.c $(SRCS)/c4cc/asm-c4r.c
C4KE_SRCS := load-c4r.c $(SRCS)/c4ke/c4ke.c \
             $(SRCS)/c4ke/extensions/c4ke_ipc.c $(SRCS)/c4ke/extensions/c4ke_plus.c \
             $(SRCS)/c4ke/extensions/c4ke_pm.c
C4KE_HDRS := $(INCLUDE)/c4.h $(INCLUDE)/c4m.h
C4KE_C4R  := c4ke.c4r
BIN_D     := $(SRCS)/c4ke/bin
C4KE_BIN  := $(BIN_D)/c4le.c4r $(BIN_D)/cat.c4r $(BIN_D)/echo.c4r \
             $(BIN_D)/kill.c4r $(BIN_D)/ls.c4r $(BIN_D)/ps.c4r \
             $(BIN_D)/spin.c4r $(BIN_D)/type.c4r $(BIN_D)/xxd.c4r
U0        := $(INCLUDE)/u0.h
PS_C      := $(SRCS)/c4ke/bin/ps.c
ESHELL_C  := $(SRCS)/c4ke/bin/eshell.c
INIT_SRCS := $(U0) $(PS_C) $(ESHELL_C) $(SRCS)/c4ke/services/init.c
INIT      := init.c4r
C4SH_D    := $(SRCS)/c4sh
C4SH_SRCS := $(U0) $(PS_C) $(C4SH_D)/c4sh.c $(C4SH_D)/c4sh_builtins.c $(C4SH_D)/c4sh_scripting.c
C4SH      := c4sh.c4r
VFS       := c4ke.vfs.c4r
SERVICE_H := $(SRCS)/c4ke/include/service.h
VFS_SRCS  := $(U0) $(SERVICE_H) $(SRCS)/c4ke/services/c4ke.vfs.c
C4R_TOP   := top.c4r
C4R_C4CC  := c4cc.c4r
C4R_C4RDUMP := c4rdump.c4r
C4R_C4RLINK := c4rlink.c4r
BENCHS    := $(SRCS)/bench/bench.c4r $(SRCS)/bench/benchtop.c4r $(SRCS)/bench/innerbench.c4r
TESTS     := src/tests
TESTS_C4R := $(TESTS)/hello.c4r $(TESTS)/mandel.c4r $(TESTS)/factorial.c4r $(TESTS)/fun_with_ptrs.c4r \
             $(TESTS)/multifun.c4r $(TESTS)/test-order.c4r $(TESTS)/test-ptrs.c4r $(TESTS)/test_args.c4r \
			 $(TESTS)/test_basic.c4r $(TESTS)/test_crash.c4r $(TESTS)/test_customop.c4r $(TESTS)/test_exit.c4r \
			 $(TESTS)/test_fread.c4r $(TESTS)/test_infiniteloop.c4r \
			 $(TESTS)/test_malloc.c4r $(TESTS)/test_printf.c4r $(TESTS)/test_printloop.c4r \
			 $(TESTS)/test_signal.c4r $(TESTS)/test_static.c4r $(TESTS)/tests.c4r \
			 $(TESTS)/rps.c4r $(TESTS)/test_continue.c4r $(TESTS)/test_timekeeping.c4r \
			 $(TESTS)/test_float.c4r $(TESTS)/test_vprintf.c4r
BIN       := c4.c4r $(C4R_C4CC) $(C4R_C4RDUMP) $(C4R_C4RLINK) $(C4R_TOP) \
            $(C4M).c4r \
            $(C4KE_C4R) \
            $(INIT) \
            $(VFS) \
            $(C4SH) \
            $(TESTS_C4R) \
			$(C4KE_BIN) \
			$(BENCHS)
C4RS     := $(BIN)
RUN_C4KE := load-c4r.c -- $(C4KE_C4R)
TEST_MASSIVE_NUM := 20

# Compile a C file with the native compiler
define compile_c
	$(eval $@_source = $(1))
	$(eval $@_out    = $(2))
	@# TODO: Make -lm an optional component
	$(NATIVE_CC) $(NATIVE_CC_OPTS) ${$@_source} -o ${$@_out} -lm
endef

# Default target
all: pre

# Prerequisites: the native C4 and related binaries, plus C4KE, and supporting binaries
pre: $(C4) $(C4M) $(C4CC) $(C4RDUMP) $(C4RLINK) $(C4RS) $(BIN_D) $(BENCHS)
	cp $(BIN_D)/*.c4r .
	cp $(SRCS)/bench/*.c4r .
	cp $(SRCS)/tests/*.c4r .

# Standard targets
run: pre
	$(C4M) $(RUN_C4KE)
run-vg: pre
	valgrind $(C4M) $(RUN_C4KE)
run-alt: pre
	$(C4M) -a $(RUN_C4KE)
run-alt-vg: pre
	valgrind $(C4M) -a $(RUN_C4KE)
run-c4: pre
	$(C4) $(C4M).c $(RUN_C4KE)
run-c4-vg: pre
	valgrind $(C4) $(C4M).c $(RUN_C4KE)
run-c4-alt: pre
	$(C4) $(C4M).c -a $(RUN_C4KE)
run-c4-alt-vg: pre
	valgrind $(C4) $(C4M).c -a $(RUN_C4KE)
test: pre
	$(C4M) $(RUN_C4KE) innerbench
test-alt: pre
	$(C4M) -a $(RUN_C4KE) innerbench
test-c4: pre
	$(C4) $(C4M).c $(RUN_C4KE) innerbench
test-c4-alt: pre
	$(C4) $(C4M) -a $(RUN_C4KE) innerbench
test-massive: pre
	$(C4M) $(RUN_C4KE) innerbench -n $(TEST_MASSIVE_NUM)
test-massive-alt: pre
	$(C4M) -a $(RUN_C4KE) innerbench -n $(TEST_MASSIVE_NUM)
test-massive-c4: pre
	$(C4) $(C4M).c $(RUN_C4KE) innerbench -n $(TEST_MASSIVE_NUM)
test-massive-c4-alt: pre
	$(C4) $(C4M) -a $(RUN_C4KE) innerbench -n $(TEST_MASSIVE_NUM)
# c4sp, the Lisp interpreter (docs/c4sp-design.md)
C4SP_SRCS := src/c4sp/c4sp.c src/c4sp/include/cell.h src/c4sp/include/gc.h \
             src/c4sp/include/cells.h src/c4sp/include/atoms.h \
             src/c4sp/include/read.h src/c4sp/include/stdlib.h \
             src/c4sp/include/eval.h src/c4sp/include/cek.h \
             include/c4_float.h
# -O0 is load-bearing: the collector finds roots by scanning the stack, and
# an optimizing gcc may keep the only reference to a cell in a register.
# Under the C4 VM the scan is exact; this caveat is native-only.
c4sp: $(C4SP_SRCS)
	gcc $(EXTRA_CC) -O0 -g -Iinclude -I. -o c4sp src/c4sp/c4sp.c
c4sp.c4r: $(C4CC) $(C4SP_SRCS)
	$(PREPROC) src/c4sp/c4sp.c | $(C4CC) -o c4sp.c4r -
# c4sp test, three parts:
#  1. the canonical written form of each sample must survive a
#     parse -> print -> parse -> print round trip;
#  2. evaluating each sample must match src/c4sp/tests/expected/, whose
#     contents were verified against the Node alisp build (see the README
#     there for the one deliberate divergence);
#  3. the c4r build under c4m must agree with the native build.
# seval gets a larger arena until the M2 collector lands.
test-c4sp: c4sp c4sp.c4r c4m $(C4KE_C4R)
	for f in src/c4sp/lisp/*.lisp; do \
		./c4sp -p $$f > .c4sp_rt1 || exit 1; \
		./c4sp -p .c4sp_rt1 > .c4sp_rt2 || exit 1; \
		cmp .c4sp_rt1 .c4sp_rt2 || exit 1; \
		./c4m load-c4r.c -- c4sp.c4r -p $$f | cmp - .c4sp_rt1 || exit 1; \
	done
	rm -f .c4sp_rt1 .c4sp_rt2
	for t in fac listadd macros quote set switch arguments; do \
		./c4sp src/c4sp/lisp/$$t.lisp | cmp - src/c4sp/tests/expected/$$t.txt || exit 1; \
		./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/$$t.lisp | cmp - src/c4sp/tests/expected/$$t.txt || exit 1; \
	done
	./c4sp src/c4sp/lisp/fac.lisp 12 | cmp - src/c4sp/tests/expected/fac-12.txt
	./c4sp src/c4sp/lisp/seval.lisp | cmp - src/c4sp/tests/expected/seval.txt
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/seval.lisp | cmp - src/c4sp/tests/expected/seval.txt
	./c4sp src/c4sp/lisp/gcloop.lisp | cmp - src/c4sp/tests/expected/gcloop.txt
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/gcloop.lisp 20000 | cmp - src/c4sp/tests/expected/gcloop.txt
	./c4sp src/c4sp/lisp/seval.lisp -s src/c4sp/lisp/fac.lisp | cmp - src/c4sp/tests/expected/seval-fac.txt
	./c4sp src/c4sp/lisp/seval.lisp -s src/c4sp/lisp/macros.lisp | cmp - src/c4sp/tests/expected/seval-macros.txt
	./c4sp src/c4sp/lisp/seval.lisp -s -t src/c4sp/lisp/seval.lisp -s src/c4sp/lisp/fac.lisp | cmp - src/c4sp/tests/expected/seval-seval-fac.txt
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/seval.lisp -s src/c4sp/lisp/fac.lisp | cmp - src/c4sp/tests/expected/seval-fac.txt
	# CEK vs the recursive reference evaluator (-R): identical output on
	# every sample, and the depth test only the CEK machine survives under
	# the C4 VM (the recursive evaluator needs a C4 stack frame per level).
	for f in src/c4sp/lisp/*.lisp; do \
		./c4sp $$f > .c4sp_cek 2>&1; ./c4sp -R $$f > .c4sp_rec 2>&1; \
		cmp .c4sp_cek .c4sp_rec || exit 1; \
	done
	rm -f .c4sp_cek .c4sp_rec
	./c4sp -R src/c4sp/lisp/seval.lisp | cmp - src/c4sp/tests/expected/seval.txt
	./c4sp src/c4sp/lisp/deeprec.lisp | cmp - src/c4sp/tests/expected/deeprec.txt
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/deeprec.lisp | cmp - src/c4sp/tests/expected/deeprec.txt
	# M4: floats (binary32), the REPL, and c4sp as a C4KE process
	./c4sp src/c4sp/lisp/floats.lisp | cmp - src/c4sp/tests/expected/floats.txt
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/floats.lisp | cmp - src/c4sp/tests/expected/floats.txt
	printf '(+ 1 2)\n(* 3.5 2)\n(define x 7)\n(+ x 0.5)\n' | ./c4sp -i | cmp - src/c4sp/tests/expected/repl.txt
	./c4m load-c4r.c -- $(C4KE_C4R) c4sp.c4r src/c4sp/lisp/fac.lisp | grep -q "Factorial of 10 = 3628800"
	# M5: .c4r -> labelled instruction lists -> .c4r, byte-identical, and
	# the re-encoded image still runs
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp hello.c4r .c4sp_rt.c4r | grep -q "roundtrip identical"
	cmp hello.c4r .c4sp_rt.c4r
	./c4m load-c4r.c -- .c4sp_rt.c4r | grep -q yello
	rm -f .c4sp_rt.c4r
	./c4sp -c 2000000 src/c4sp/lisp/c4r-roundtrip.lisp test_float.c4r | grep -q "roundtrip identical"
	./c4sp -c 8000000 src/c4sp/lisp/c4r-roundtrip.lisp c4sp.c4r | grep -q "roundtrip identical"
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/c4r-roundtrip.lisp hello.c4r | grep -q "roundtrip identical"
	@echo "test-c4sp: OK"

# The heavyweight version: seval evaluating seval evaluating fac, under c4m
# (about a minute of interpreted interpretation of an interpreter).
test-c4sp-deep: c4sp.c4r c4m
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/seval.lisp -s -t src/c4sp/lisp/seval.lisp -s src/c4sp/lisp/fac.lisp | cmp - src/c4sp/tests/expected/seval-seval-fac.txt
	@echo "test-c4sp-deep: OK"

# Linking test: compile two modules separately, link both ways, run each,
# and exercise library mode (-r) with a relink of the written library.
test-link: c4m $(C4CC) $(C4RLINK)
	$(C4CC) -o tla.c4o src/tests/test_link_a.c
	$(C4CC) -o tlb.c4o src/tests/test_link_b.c
	$(C4RLINK) tla.c4o tlb.c4o -o test_link.c4r
	$(C4M) load-c4r.c -- test_link.c4r
	$(C4RLINK) tlb.c4o tla.c4o -o test_link2.c4r
	$(C4M) load-c4r.c -- test_link2.c4r
	$(C4RLINK) -r tla.c4o -o tla.c4l
	$(C4RLINK) tla.c4l tlb.c4o -o test_link3.c4r
	$(C4M) load-c4r.c -- test_link3.c4r
	rm -f tla.c4o tlb.c4o tla.c4l test_link.c4r test_link2.c4r test_link3.c4r
	@echo "test-link: OK"
clean-c4rs:
	rm -rf $(C4RS) $(BIN) *.c4r c4ke.pre.c
clean: clean-c4rs
	rm -rf $(C4) $(C4M) $(C4CC)
pkg:
	tar cjf $(PKG) c4ke.vfs.txt *.c src include Makefile
# These rules are for personal testing
CCOR1K      := /opt/or1k-linux-musl/bin/or1k-musl-linux-gcc
CFLAGSOR1K  := -DOPENRISC -O2 -g -idirafter include -I .
DEP_FLAGS    =
OR1K_TGZ := c4-or1k-new.tgz
DEST_TGZ := ~/git/jorconsole/jor1k-sysroot/fs/home/user/c4.tgz
or1k: clean
	# Cross-compile the various executables
	$(CCOR1K) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGSOR1K) -c c4.c -o c4.o
	$(CCOR1K) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGSOR1K) -c c4m.c -o c4m.o
	$(CCOR1K) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGSOR1K) -c c4m_float.c -o c4m_float.o
	$(CCOR1K) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGSOR1K) -c src/c4cc/asm-c4r.c -o c4cc.o
	$(CCOR1K) $(CFLAGS) $(LDFLAGS) c4.o $(LDLIBS) -o c4
	$(CCOR1K) $(CFLAGS) $(LDFLAGS) c4m.o $(LDLIBS) c4m_float.o -o c4m
	$(CCOR1K) $(CFLAGS) $(LDFLAGS) c4cc.o $(LDLIBS) -o c4cc
	tar cjf $(OR1K_TGZ) c4ke.vfs.txt $(C4) *.c $(C4M) $(C4CC) include src Makefile or1k.sh *.o
	cp $(OR1K_TGZ) $(DEST_TGZ)
# Old method of building prerequisites
c4rs: pre

# Marking the below rules as PHONY using singular .PHONY rule
PHONY  = pre all clean-c4rs clean
PHONY += run run-vg test test-massive
PHONY += run-alt run-alt-vg test-alt test-massive-alt
PHONY += run-c4 run-c4-vg test-c4 test-massive-c4
PHONY += run-c4-alt run-c4-alt-vg
PHONY += pkg c4rs or1k
PHONY += pi
# Don't bother with the dump or link utility for now
# PHONY += $(C4RDUMP) $(C4RLINK)
.PHONY: $(PHONY)

#
# Rules to build native versions of c4, c4m, c4cc, and tools
#
c4: c4.c
	$(call compile_c,$<,$@)
c4m: c4m.c
	gcc $(EXTRA_CC) -O2 -g -idirafter include -I . c4m.c c4m_float.c -o c4m -lm
c4cc: $(C4CC_SRCS)
	$(call compile_c,src/c4cc/asm-c4r.c,c4cc)
$(C4RDUMP): src/c4ke/bin/c4rdump.c load-c4r.c
#gcc $(EXTRA_CC) -O2 -g -Isrc/c4cc -I include -I . src/c4ke/bin/c4rdump.c -o $(C4RDUMP)
	$(NATIVE_CC) $(NATIVE_CC_OPTS) -Isrc/c4cc src/c4ke/bin/c4rdump.c -o $(C4RDUMP)
$(C4RLINK): src/c4ke/bin/c4rlink.c src/c4cc/asm-c4r.c src/c4cc/c4cc.c load-c4r.c
	gcc $(EXTRA_CC) -O2 -g -Isrc/c4cc -I include -I . src/c4ke/bin/c4rlink.c -o $(C4RLINK)

#
# Rules to build C4R files
#

c4.c4r: c4.c $(U0) $(C4CC)
	$(C4CC) -o c4.c4r $(U0) c4.c
c4m.c4r: c4m.c $(U0) include/c4m.h $(C4CC)
	# $(C4CC) -o c4m.c4r $(U0) c4m.c
	$(PREPROC) c4m.c | $(C4CC) -o c4m.c4r -

# C4KE - The C4 Kernel Experiment, and supporting files
$(C4KE_C4R): $(C4CC)
	$(PREPROC) src/c4ke/c4ke.c | $(C4CC) -o $(C4KE_C4R) -
# The init process
$(INIT): $(C4CC) $(INIT_SRCS)
	$(C4CC) -o $(INIT) $(INIT_SRCS)
# The C4 Shell
$(C4SH): $(C4CC) $(C4SH_SRCS)
	$(C4CC) -o $(C4SH) $(C4SH_SRCS)
# C4KE VFS
$(VFS): $(C4CC) $(VFS_SRCS)
	$(C4CC) -o $(VFS) $(VFS_SRCS)

# Binaries that run under C4KE, and have C4KE as a dependency so that any
# changes cause a recompile.

C4KE_WATCH := $(C4KE_SRCS) $(C4KE_HDRS)
# top, requires ps.c
$(C4R_TOP): $(C4CC) $(U0) $(SRCS)/c4ke/bin/ps.c $(SRCS)/c4ke/bin/top.c $(C4KE_WATCH)
	$(C4CC) -o $(C4R_TOP) $(U0) $(SRCS)/c4ke/bin/ps.c $(SRCS)/c4ke/bin/top.c
# C4KE version of c4cc
$(C4R_C4CC): $(C4CC) $(U0) load-c4r.c $(SRCS)/c4cc/c4cc.c $(SRCS)/c4cc/asm-c4r.c
	$(C4CC) -o $(C4R_C4CC) $(C4R_C4CC_SRCS)
# c4rdump, requires c4cc sources until proper headers implemented
$(C4R_C4RDUMP): $(C4CC) $(U0) $(C4R_C4CC_SRCS) $(SRCS)/c4ke/bin/c4rdump.c
	$(C4CC) -o $(C4R_C4RDUMP) $(C4R_C4CC_SRCS) $(SRCS)/c4ke/bin/c4rdump.c
# c4rlink, same as above
$(C4R_C4RLINK): $(C4CC) $(U0) $(C4R_C4CC_SRCS) $(SRCS)/c4ke/bin/c4rlink.c
	$(C4CC) -o $(C4R_C4RLINK) $(C4R_C4CC_SRCS) $(SRCS)/c4ke/bin/c4rlink.c
# The various binaries in c4ke/bin
$(BIN_D)/%.c4r: $(SRCS)/c4ke/bin/%.c $(C4KE_WATCH) $(C4CC)
	$(C4CC) -o $@ $(U0) $<
# The benchmarks
$(SRCS)/bench/%.c4r: $(SRCS)/bench/%.c $(C4KE_WATCH) $(C4CC)
	$(C4CC) -o $@ $(U0) $<
#
# A variety of test programs
#
# Exclusive rule: this test program doesn't link with u0
src/tests/hello.c4r: $(C4CC) $(TESTS)/hello.c
	$(C4CC) -o src/tests/hello.c4r $(TESTS)/hello.c
# Exclusive rules: these tests include real headers, so they need the
# preprocessor rather than being handed straight to c4cc (which skips # lines).
$(TESTS)/test_float.c4r: $(C4CC) $(TESTS)/test_float.c $(TESTS)/float_cases.h include/c4_float.h $(U0)
	$(PREPROC) -I$(TESTS) $(U0) $(TESTS)/test_float.c | $(C4CC) -o $@ -
$(TESTS)/test_vprintf.c4r: $(C4CC) $(TESTS)/test_vprintf.c include/stdio.h $(U0)
	$(PREPROC) $(U0) $(TESTS)/test_vprintf.c | $(C4CC) -o $@ -
# All tests should compile with the following invocation
$(SRCS)/tests/%.c4r: $(SRCS)/tests/%.c $(C4KE_WATCH) $(C4CC)
	$(C4CC) -o $@ $(U0) $<
# Build the u0 library for linking (see also: make test-link).
%.c4l: %.c $(C4KE_WATCH) $(C4CC)
	$(C4CC) -o $@ $<

pi:
	(cd .. && tar cjvf pi.tgz c4ke/*.c c4ke/include c4ke/src c4ke/Makefile c4ke/*.txt)
