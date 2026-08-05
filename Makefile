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
			 $(TESTS)/test_float.c4r $(TESTS)/test_vprintf.c4r \
			 $(TESTS)/test_ramfs.c4r $(TESTS)/test_selfhost.c4r $(TESTS)/test_ramopt.c4r \
			 $(TESTS)/test_ramcc.c4r $(TESTS)/cycles.c4r
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
# Plain c4 runs c4m from SOURCE. c4.c is the base VM and stays
# unmodified: it knows only the original opcode set, so it cannot run
# a c4cc-compiled image directly -- and it does not need to, because
# interpreting c4m.c gives the extended VM, and with it a real clock
# (c4m's own c4m_time(), not something faked in the interpreter).
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
	$(C4) $(C4M).c -a $(RUN_C4KE) innerbench
test-massive: pre
	$(C4M) $(RUN_C4KE) innerbench -n $(TEST_MASSIVE_NUM)
test-massive-alt: pre
	$(C4M) -a $(RUN_C4KE) innerbench -n $(TEST_MASSIVE_NUM)
test-massive-c4: pre
	$(C4) $(C4M).c $(RUN_C4KE) innerbench -n $(TEST_MASSIVE_NUM)
test-massive-c4-alt: pre
	$(C4) $(C4M).c -a $(RUN_C4KE) innerbench -n $(TEST_MASSIVE_NUM)
# c4l.c: a .c4r loader for PLAIN c4. c4.c is the unmodified base VM,
# so this runs images that stay inside the original opcode set --
# which is exactly the boundary worth pinning. An image using the
# extended opcodes (here, c4cc's switch jumptables) must be refused
# by plain c4 and must run under c4m through load-c4r.c. Both halves
# are checked, because the interesting property is that the fallback
# degrades cleanly rather than misbehaving.
test-c4l: $(C4) $(C4M) $(C4CC) $(TESTS)/hello.c4r
	$(C4) c4l.c $(TESTS)/hello.c4r | grep -q yello
	$(C4CC) -o .c4l_sw.c4r $(TESTS)/test_switch.c
	$(C4) c4l.c .c4l_sw.c4r 2>&1 | grep -q "unknown instruction"
	$(C4M) load-c4r.c -- .c4l_sw.c4r | grep -q "classify(5) = 500"
	rm -f .c4l_sw.c4r
	@echo "test-c4l: OK"

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
	for t in fac listadd macros quote set switch arguments truthy; do \
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
	# every sample except callcc.lisp, which the recursive evaluator
	# rejects by design. The depth test only the CEK machine survives
	# under the C4 VM (the recursive evaluator needs a stack frame per
	# level).
	for f in src/c4sp/lisp/*.lisp; do \
		case $$f in */callcc.lisp) continue;; esac; \
		./c4sp $$f > .c4sp_cek 2>&1; ./c4sp -R $$f > .c4sp_rec 2>&1; \
		cmp .c4sp_cek .c4sp_rec || exit 1; \
	done
	rm -f .c4sp_cek .c4sp_rec
	# call/cc: CEK only; -R must refuse it rather than misbehave
	./c4sp src/c4sp/lisp/callcc.lisp | cmp - src/c4sp/tests/expected/callcc.txt
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/callcc.lisp | cmp - src/c4sp/tests/expected/callcc.txt
	./c4sp -R src/c4sp/lisp/callcc.lisp 2>&1 | grep -q "requires the CEK machine"
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

# M6: the optimizer. Optimize factorial.c4r, run original and optimized
# under C4KE, and require identical output (kernel chatter and the startup
# stacktrace demo filtered out). Then optimize the compiler itself and
# require an identical -S listing (modulo the absolute pool addresses the
# listing prints, which differ between any two runs).
test-c4sp-opt: c4sp c4sp.c4r c4m $(C4KE_C4R)
	./c4sp -c 4000000 src/c4sp/lisp/c4opt-run.lisp factorial.c4r .c4sp_opt.c4r
	./c4m load-c4r.c -- $(C4KE_C4R) factorial.c4r 2>&1 | grep -v "^c4ke\|^lc4r\|stacktrace\|Have a nice" > .c4sp_opt_a
	./c4m load-c4r.c -- $(C4KE_C4R) .c4sp_opt.c4r 2>&1 | grep -v "^c4ke\|^lc4r\|stacktrace\|Have a nice" > .c4sp_opt_b
	cmp .c4sp_opt_a .c4sp_opt_b
	./c4sp -c 16000000 src/c4sp/lisp/c4opt-run.lisp $(C4R_C4CC) .c4sp_opt_cc.c4r
	./c4m load-c4r.c -- $(C4R_C4CC) -S src/tests/multifun.c 2>&1 | sed -E 's/[0-9]{9,}/ADDR/g' > .c4sp_opt_a
	./c4m load-c4r.c -- .c4sp_opt_cc.c4r -S src/tests/multifun.c 2>&1 | sed -E 's/[0-9]{9,}/ADDR/g' > .c4sp_opt_b
	cmp .c4sp_opt_a .c4sp_opt_b
	rm -f .c4sp_opt.c4r .c4sp_opt_cc.c4r .c4sp_opt_a .c4sp_opt_b
	# The tail pass: a million mutual zero-arg tail calls overflow the VM
	# stack unoptimized; the frame-reuse rewrite runs them flat.
	$(C4CC) -o .c4sp_tail.c4r src/tests/test_tailcall.c
	./c4sp src/c4sp/lisp/c4opt-run.lisp .c4sp_tail.c4r .c4sp_tail_opt.c4r | grep -q " tail 2"
	./c4m load-c4r.c -- .c4sp_tail_opt.c4r | grep -q "parity 0 counter 1000000"
	rm -f .c4sp_tail.c4r .c4sp_tail_opt.c4r
	# initialized globals and arrays (test_globals expected output was
	# generated by gcc from the same source): behaviour, byte-identical
	# round trip of the data-resident patches, identical after optimization
	$(C4CC) -o .c4sp_glob.c4r src/tests/test_globals.c
	./c4m load-c4r.c -- .c4sp_glob.c4r | cmp - src/c4sp/tests/expected/test_globals.txt
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp .c4sp_glob.c4r | grep -q "roundtrip identical"
	./c4sp src/c4sp/lisp/c4opt-run.lisp .c4sp_glob.c4r .c4sp_glob_opt.c4r > /dev/null
	./c4m load-c4r.c -- .c4sp_glob_opt.c4r | cmp - src/c4sp/tests/expected/test_globals.txt
	rm -f .c4sp_glob.c4r .c4sp_glob_opt.c4r
	# switch (c4cc jump tables, now in the data segment): correct
	# behaviour, byte-identical round trip, identical after optimization
	$(C4CC) -o .c4sp_sw.c4r src/tests/test_switch.c
	./c4m load-c4r.c -- .c4sp_sw.c4r | cmp - src/c4sp/tests/expected/test_switch.txt
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp .c4sp_sw.c4r | grep -q "roundtrip identical"
	./c4sp src/c4sp/lisp/c4opt-run.lisp .c4sp_sw.c4r .c4sp_sw_opt.c4r > /dev/null
	./c4m load-c4r.c -- .c4sp_sw_opt.c4r | cmp - src/c4sp/tests/expected/test_switch.txt
	rm -f .c4sp_sw.c4r .c4sp_sw_opt.c4r
	@echo "test-c4sp-opt: OK"

# c4lc, the C compiler written in c4sp Lisp (docs/c4lc-design.md).
# The L3 differential battery: raw tests both compilers build, compared
# byte-for-byte (MASKED = prints runtime addresses, pointers stripped
# first; PP = variadic, preprocessed so stdarg.h is inlined).
C4LC_DIFF := c4_jailbreak factorial hello multifun puts reverse \
             test_basic test_continue test_coop_switch test_globals \
             test_malloc test-order test-ptrs test_static test_switch \
             tests c4lc_l2
C4LC_DIFF_MASKED := global test_gcscan test-oisc test_printf
C4LC_DIFF_PP := vararg2 test_vprintf
C4LC_LISP := src/c4sp/lisp/c4lc.lisp src/c4sp/lisp/c4lc-lex.lisp \
             src/c4sp/lisp/c4lc-parse.lisp src/c4sp/lisp/c4lc-gen.lisp \
             src/c4sp/lisp/c4lc-tree.lisp src/c4sp/lisp/c4r.lisp \
             src/c4sp/lisp/c4opt.lisp

# c4lc -O builds of the three big images (L6). Each is smaller and
# never slower than its c4cc twin; see docs/c4lc-design.md 12 for
# measurements. Not part of the default build -- swap them in by
# copying over c4ke.c4r / c4sp.c4r / c4m.c4r.
c4ke-lc.c4r: c4sp $(C4LC_LISP) $(SRCS)/c4ke/c4ke.c
	$(PREPROC) $(SRCS)/c4ke/c4ke.c > .c4lc_klc.c
	./c4sp -c 32000000 src/c4sp/lisp/c4lc.lisp -O .c4lc_klc.c c4ke-lc.c4r
	rm -f .c4lc_klc.c
c4sp-lc.c4r: c4sp $(C4LC_LISP) $(C4SP_SRCS)
	$(PREPROC) src/c4sp/c4sp.c > .c4lc_klc.c
	./c4sp -c 16000000 src/c4sp/lisp/c4lc.lisp -O .c4lc_klc.c c4sp-lc.c4r
	rm -f .c4lc_klc.c
c4m-lc.c4r: c4sp $(C4LC_LISP) c4m.c
	$(PREPROC) c4m.c > .c4lc_klc.c
	./c4sp -c 16000000 src/c4sp/lisp/c4lc.lisp -O .c4lc_klc.c c4m-lc.c4r
	rm -f .c4lc_klc.c

# OISC4: the One Instruction Set Computer (docs/oisc4-design.md).
# Runs .c4r images on a single-instruction VM; verified bit-identical
# against the c4m loader by test-oisc4.
OISC4 := src/oisc4/oisc4
$(OISC4): src/oisc4/oisc4.c
	$(NATIVE_CC) $(NATIVE_CC_OPTS) src/oisc4/oisc4.c -o $(OISC4)
oisc4: $(OISC4)
# OISC4 compiled by c4lc: runs nested under c4m, plain c4 (via c4l.c),
# or oisc4 itself (OISC on OISC).
oisc4-lc.c4r: c4sp $(C4LC_LISP) src/oisc4/oisc4.c
	$(PREPROC) src/oisc4/oisc4.c > .c4lc_o4.c
	./c4sp -c 16000000 src/c4sp/lisp/c4lc.lisp -O .c4lc_o4.c oisc4-lc.c4r
	rm -f .c4lc_o4.c
test-oisc4: $(OISC4) $(C4M) c4.c4r c4sp.c4r $(TESTS_C4R)
	bash src/oisc4/test-oisc4.sh
test-oisc4-nested: $(OISC4) $(C4) $(C4M) oisc4-lc.c4r $(TESTS)/hello.c4r
	$(C4M) load-c4r.c -- oisc4-lc.c4r -m 32 $(TESTS)/hello.c4r | grep -q yello
	$(C4) c4l.c oisc4-lc.c4r -m 32 $(TESTS)/hello.c4r | grep -q yello
	$(OISC4) -m 192 oisc4-lc.c4r -m 32 $(TESTS)/hello.c4r | grep -q yello
	@echo "test-oisc4-nested: OK"

# C4IX (docs/c4ix-design.md): the c4lc-compiled OS. Each module is
# preprocessed, compiled to a .c4o object with full optimization, and
# the kernel image is linked by c4rlink.
C4IX_SRC  := src/c4ix
C4IX_MODS := boot con va host sl4b task sched vfs sys loader init
c4ix.c4r: c4sp $(C4RLINK) $(C4LC_LISP) $(C4IX_SRC)/c4ix.h $(patsubst %,$(C4IX_SRC)/%.c,$(C4IX_MODS))
	@# No gcc here: c4lc preprocesses the modules itself (L9). Each
	@# object is byte-identical to the gcc -E path, pinned by test-c4lc.
	for m in $(C4IX_MODS); do \
		./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O -c -I $(C4IX_SRC) $(C4IX_SRC)/$$m.c .c4ix_$$m.c4o > /dev/null || exit 1; \
	done
	$(C4RLINK) $(patsubst %,.c4ix_%.c4o,$(C4IX_MODS)) -o c4ix.c4r
	rm -f .c4ix_*.pp.c .c4ix_*.c4o

# the spawn-test userland program: raw opcodes, no library. Under
# protected mode its printf traps and the kernel emulates it onto the
# fd layer -- redirection for programs that never heard of C4IX.
c4ix-hello.c4r: c4sp $(C4LC_LISP) $(C4IX_SRC)/user/hello.c
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O $(C4IX_SRC)/user/hello.c c4ix-hello.c4r > /dev/null

# libc4ix, the userland C library, as a c4rlink archive
libc4ix.c4l: c4sp $(C4RLINK) $(C4LC_LISP) $(C4IX_SRC)/lib/libc4ix.c $(C4IX_SRC)/include/c4ix_user.h
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O -c -I $(C4IX_SRC)/include $(C4IX_SRC)/lib/libc4ix.c .c4ix_lib.c4o > /dev/null
	$(C4RLINK) -r .c4ix_lib.c4o -o libc4ix.c4l
	rm -f .c4ix_lib.pp.c .c4ix_lib.c4o

# userland programs built against the library: all IO via syscalls
c4ix-%.c4r: c4sp $(C4RLINK) $(C4LC_LISP) libc4ix.c4l $(C4IX_SRC)/user/%.c
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O -c -I $(C4IX_SRC)/include $(C4IX_SRC)/user/$*.c .c4ix_u.c4o > /dev/null
	$(C4RLINK) .c4ix_u.c4o libc4ix.c4l -o $@
	rm -f .c4ix_u.pp.c .c4ix_u.c4o

# X3 boot pins: the linked kernel boots natively on c4m (preemptive,
# user tasks behind protected mode) and degraded on plain c4 through
# the c4l loader (cooperative, no hardware boundary). Output is exact
# per host; the differences are by design and worth reading:
# c4ix-hello.c4r calls printf directly, so on c4m it traps and the
# kernel emulates it onto the fd layer -- which is why redirecting it
# into a RAM file captures 58 bytes there and 0 on plain c4, where a
# raw printf has no boundary to cross. The pipeline (uecho | uwc)
# works on both.
C4IX_PROGS := c4ix-hello.c4r c4ix-uhello.c4r c4ix-echo.c4r c4ix-wc.c4r \
              c4ix-cat.c4r c4ix-sh.c4r c4ix-ps.c4r c4ix-bench.c4r \
              c4ix-cycles.c4r c4ix-ls.c4r c4ix-mkdir.c4r c4ix-top.c4r \
              c4ix-spin.c4r c4ix-fmt.c4r
# The demonstrations are opt-in (--demo) and take their six images
# positionally; `c4ix.c4r --help` documents the roles. With no
# arguments at all the kernel boots to a shell, which is what
# run-c4ix relies on.
C4IX_ARGS := --demo c4ix-hello.c4r c4ix-uhello.c4r c4ix-echo.c4r c4ix-wc.c4r \
             c4ix-sh.c4r $(C4IX_SRC)/user/test.sh
# Cycle counts are masked: they are a measurement, not behaviour, and
# they move whenever the kernel's size changes. `make bench-c4ix`
# reports them unmasked.
C4IX_MASK := sed -E 's/[0-9]+ cycles/N cycles/g'
# C4IX targets c4m. It is not a plain-c4 program and does not try to
# be: its images use the extended opcodes (C4CY, PUTC) and indirect
# calls (JSRI/JSRS) that the base VM does not have. c4m is the layer
# that degrades to plain c4 -- being itself a c4 program -- so
# "C4IX under plain c4" means c4 -> c4m -> C4IX, which is what
# test-c4ix-c4 below runs. In that chain C4IX sees a c4m host and
# behaves identically, so it compares against the same pin.
test-c4ix: c4m c4ix.c4r $(C4IX_PROGS)
	$(C4M) load-c4r.c -- c4ix.c4r $(C4IX_ARGS) | $(C4IX_MASK) | cmp - $(C4IX_SRC)/tests/x5-c4m.txt
	@echo "test-c4ix: OK"
# There are two printf implementations a C4IX program can reach --
# libc4ix formats in userland, while a raw printf traps and the KERNEL
# formats it -- and output must not depend on which one was used.
# c4ix-fmt prints every case through both; this extracts the two sets
# and compares them, so drift is a diff rather than a surprise. The
# pin then fixes what the shared answer actually is.
test-c4ix-fmt: c4m c4ix.c4r c4ix-fmt.c4r
	@$(C4M) load-c4r.c -- c4ix.c4r c4ix-fmt.c4r > .c4ix_fmt.out
	@sed -n 's/^u|//p' .c4ix_fmt.out > .c4ix_fmt.u
	@sed -n 's/^p|//p' .c4ix_fmt.out > .c4ix_fmt.p
	cmp .c4ix_fmt.u .c4ix_fmt.p
	cmp .c4ix_fmt.u $(C4IX_SRC)/tests/fmt.txt
	@rm -f .c4ix_fmt.out .c4ix_fmt.u .c4ix_fmt.p
	@echo "test-c4ix-fmt: OK"
# The same thing with c4m itself interpreted by plain c4. Correct but
# very slow (nested interpretation), so it is not part of test-c4ix.
test-c4ix-c4: c4 c4m c4ix.c4r $(C4IX_PROGS)
	$(C4) $(C4M).c load-c4r.c -- c4ix.c4r $(C4IX_ARGS) | sed '/^exit([0-9-]*) cycle = /d' | $(C4IX_MASK) | cmp - $(C4IX_SRC)/tests/x5-c4m.txt
	@echo "test-c4ix-c4: OK"
# X5: the OS benchmark, and the one number that compares directly
# with C4KE -- cycles from VM start to userland running, same VM and
# same counter on both sides.
bench-c4ix: c4m c4ix.c4r $(C4IX_PROGS)
	@echo "--- C4IX ---"
	$(C4M) load-c4r.c -- c4ix.c4r --demo c4ix-hello.c4r c4ix-uhello.c4r \
		c4ix-echo.c4r c4ix-wc.c4r c4ix-sh.c4r $(C4IX_SRC)/user/bench.sh \
		2>&1 | grep -E "scheduling after|^bench:"
	@echo "--- boot cost, C4IX (VM cycle counter) ---"
	@$(C4M) load-c4r.c -- c4ix.c4r c4ix-cycles.c4r 2>&1 | grep -E "booting|scheduling after|^cycles:"
	@echo "--- end to end: boot a kernel and run one trivial program ---"
	@echo -n "c4ix wall: "
	@/usr/bin/time -f "%e s" $(C4M) load-c4r.c -- c4ix.c4r c4ix-cycles.c4r 2>&1 | tail -1
	@echo -n "c4ke wall: "
	@/usr/bin/time -f "%e s" $(C4M) $(RUN_C4KE) -v 9 cycles 2>&1 | tail -1
	@$(C4M) $(RUN_C4KE) cycles 2>&1 | grep -E "Kernel ready" || true

# An interactive shell, which is what the kernel does when given
# nothing to do: c4ix-sh with no script, so it reads fd 0 -- you.
# `help` lists builtins; `exit` or Ctrl-D leaves, which shuts the
# kernel down. `c4ix.c4r --help` lists the other modes.
run-c4ix: c4m c4ix.c4r $(C4IX_PROGS)
	$(C4M) load-c4r.c -- c4ix.c4r
run-c4ix-c4: c4 c4m c4ix.c4r $(C4IX_PROGS)
	$(C4) $(C4M).c load-c4r.c -- c4ix.c4r
# The guided tour instead: every milestone's demonstration in order,
# ending with the shell running the test script.
demo-c4ix: c4m c4ix.c4r $(C4IX_PROGS)
	$(C4M) load-c4r.c -- c4ix.c4r $(C4IX_ARGS)
demo-c4ix-c4: c4 c4m c4ix.c4r $(C4IX_PROGS)
	$(C4) $(C4M).c load-c4r.c -- c4ix.c4r $(C4IX_ARGS)
# L0: golden token dump of a sample covering every token kind and c4cc
# lexer quirk, native and under c4m, plus a full lex of c4cc.c itself
# (whose token list needs a bigger cell arena than the default).
test-c4lc: c4sp c4sp.c4r c4m $(C4CC) $(C4RLINK) $(C4KE_C4R) $(TESTS)/test_ramcc.c4r
	./c4sp src/c4sp/lisp/c4lc-tokens.lisp src/tests/c4lc_lex_sample.c | cmp - src/c4sp/tests/expected/c4lc-tokens.txt
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/c4lc-tokens.lisp src/tests/c4lc_lex_sample.c | cmp - src/c4sp/tests/expected/c4lc-tokens.txt
	./c4sp -c 2000000 src/c4sp/lisp/c4lc-tokens.lisp -count src/c4cc/c4cc.c | grep -q "^tokens [0-9]"
	# L1: AST golden of the sample, native and under c4m
	./c4sp src/c4sp/lisp/c4lc-ast.lisp src/tests/c4lc_lex_sample.c | cmp - src/c4sp/tests/expected/c4lc-ast.txt
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/c4lc-ast.lisp src/tests/c4lc_lex_sample.c | cmp - src/c4sp/tests/expected/c4lc-ast.txt
	# L1: every c4cc-compilable test source parses. genfloat.c and
	# test_illins.c are excluded because c4cc itself rejects them (c4lc
	# fails at the same constructs); c4lc_pp.c is excluded because it
	# is the L9 preprocessor test and is only meaningful with -P (this
	# sweep deliberately runs without it); the vararg tests go through
	# the preprocessor, exactly as c4cc receives them.
	for f in src/tests/*.c; do \
		case $$f in \
		*/genfloat.c|*/test_illins.c) continue;; \
		*/c4lc_pp.c) continue;; \
		*/oldtest_vararg*.c|*/oldvararg3.c|*/test_vprintf.c|*/vararg*.c) \
			$(PREPROC) $$f > .c4lc_pp.c 2>/dev/null; \
			./c4sp -c 4000000 src/c4sp/lisp/c4lc-ast.lisp -check .c4lc_pp.c | grep -q "^parse ok" || exit 1;; \
		*) \
			./c4sp -c 4000000 src/c4sp/lisp/c4lc-ast.lisp -check $$f | grep -q "^parse ok" || exit 1;; \
		esac; \
	done
	# L1: the exact self-compile unit c4cc consumes (raw concatenation,
	# no cpp -- c4cc skips '#' lines), and preprocessed c4sp.c
	cat $(U0) load-c4r.c $(SRCS)/c4cc/c4cc.c $(SRCS)/c4cc/asm-c4r.c > .c4lc_cat.c
	./c4sp -c 8000000 src/c4sp/lisp/c4lc-ast.lisp -check .c4lc_cat.c | grep -q "^parse ok"
	$(PREPROC) src/c4sp/c4sp.c > .c4lc_pp.c
	./c4sp -c 8000000 src/c4sp/lisp/c4lc-ast.lisp -check .c4lc_pp.c | grep -q "^parse ok"
	rm -f .c4lc_cat.c .c4lc_pp.c
	# L2: code generation. The L2 sample compiles under both c4cc and
	# c4lc and the two binaries must behave identically under c4m.
	# for/continue (which c4cc cannot compile: its For branch is dead
	# code) check against committed gcc-generated output. c4lc output
	# must also roundtrip through c4r.lisp byte-identically and stay
	# correct after the c4opt passes.
	$(C4CC) -o .c4lc_a.c4r src/tests/c4lc_l2.c
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp src/tests/c4lc_l2.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_a.c4r > .c4lc_out_a
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - .c4lc_out_a
	./c4sp -c 2000000 src/c4sp/lisp/c4r-roundtrip.lisp .c4lc_b.c4r | grep -q "roundtrip identical"
	./c4sp -c 2000000 src/c4sp/lisp/c4opt-run.lisp .c4lc_b.c4r .c4lc_bo.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_bo.c4r | cmp - .c4lc_out_a
	./c4sp -c 2000000 src/c4sp/lisp/c4lc.lisp src/tests/c4lc_for.c .c4lc_f.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_f.c4r | cmp - src/c4sp/tests/expected/c4lc-for.txt
	rm -f .c4lc_a.c4r .c4lc_b.c4r .c4lc_bo.c4r .c4lc_f.c4r .c4lc_out_a
	# L3: full subset. Every deterministic raw test c4cc compiles must
	# behave byte-identically when compiled by c4lc; tests that print
	# runtime addresses compare with pointers masked; the variadic
	# tests go through the preprocessor (stdarg.h + __c4cc_make_va).
	for t in $(C4LC_DIFF); do \
		$(C4CC) -o .c4lc_a.c4r src/tests/$$t.c > /dev/null 2>&1 || exit 1; \
		./c4m load-c4r.c -- .c4lc_a.c4r > .c4lc_out_a 2>&1; \
		./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp src/tests/$$t.c .c4lc_b.c4r > /dev/null || exit 1; \
		./c4m load-c4r.c -- .c4lc_b.c4r 2>&1 | cmp - .c4lc_out_a || exit 1; \
	done
	for t in $(C4LC_DIFF_MASKED); do \
		$(C4CC) -o .c4lc_a.c4r src/tests/$$t.c > /dev/null 2>&1 || exit 1; \
		./c4m load-c4r.c -- .c4lc_a.c4r 2>&1 | sed -E 's/0x[0-9a-f]+/ADDR/g' > .c4lc_out_a; \
		./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp src/tests/$$t.c .c4lc_b.c4r > /dev/null || exit 1; \
		./c4m load-c4r.c -- .c4lc_b.c4r 2>&1 | sed -E 's/0x[0-9a-f]+/ADDR/g' | cmp - .c4lc_out_a || exit 1; \
	done
	for t in $(C4LC_DIFF_PP); do \
		$(PREPROC) src/tests/$$t.c > .c4lc_pp.c 2>/dev/null; \
		$(C4CC) -o .c4lc_a.c4r .c4lc_pp.c > /dev/null 2>&1 || exit 1; \
		./c4m load-c4r.c -- .c4lc_a.c4r > .c4lc_out_a 2>&1; \
		./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp .c4lc_pp.c .c4lc_b.c4r > /dev/null || exit 1; \
		./c4m load-c4r.c -- .c4lc_b.c4r 2>&1 | cmp - .c4lc_out_a || exit 1; \
	done
	# switch images must roundtrip and survive the optimizer (the
	# jumptable's dcode label targets move with the code)
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp src/tests/test_switch.c .c4lc_b.c4r > /dev/null
	./c4sp -c 2000000 src/c4sp/lisp/c4r-roundtrip.lisp .c4lc_b.c4r | grep -q "roundtrip identical"
	./c4sp -c 2000000 src/c4sp/lisp/c4opt-run.lisp .c4lc_b.c4r .c4lc_bo.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_bo.c4r | cmp - src/c4sp/tests/expected/test_switch.txt
	# L4: -O runs the c4opt passes in-process (no intermediate file).
	# The optimized image differs from the two-step pipeline's only in
	# patch-covered operand words -- dead values the loader overwrites
	# -- so the bar is identical behavior; the tail pass lets a million
	# mutual tail calls run flat with no separate optimizer step.
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O src/tests/c4lc_l2.c .c4lc_b.c4r | grep -q "tree: folded"
	$(C4CC) -o .c4lc_a.c4r src/tests/c4lc_l2.c
	./c4m load-c4r.c -- .c4lc_a.c4r > .c4lc_out_a
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - .c4lc_out_a
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O src/tests/test_switch.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/test_switch.txt
	./c4sp -c 2000000 src/c4sp/lisp/c4r-roundtrip.lisp .c4lc_b.c4r | grep -q "roundtrip identical"
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O src/tests/test_tailcall.c .c4lc_b.c4r | grep -q " tail 2"
	./c4m load-c4r.c -- .c4lc_b.c4r | grep -q "parity 0 counter 1000000"
	# L7: structs, unions, typedef, member access, do-while, compound
	# assignment, block-scoped declarations with expression
	# initializers. gcc generated the expected output; c4cc cannot
	# compile any of this.
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp src/tests/c4lc_l7.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-l7.txt
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O src/tests/c4lc_l7.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-l7.txt
	./c4sp -c 2000000 src/c4sp/lisp/c4r-roundtrip.lisp .c4lc_b.c4r | grep -q "roundtrip identical"
	# L8: object mode. -c leaves undefined prototypes as SYMBOL-typed
	# patches with extern symbol entries for c4rlink. c4lc objects link
	# with c4cc objects in either direction; an L7 struct program built
	# from separately compiled -O objects matches both the whole-program
	# compile and committed gcc output.
	$(C4CC) -o .c4lc_oa1.c4o $(TESTS)/test_link_a.c > /dev/null 2>&1
	$(C4CC) -o .c4lc_ob1.c4o $(TESTS)/test_link_b.c > /dev/null 2>&1
	$(C4RLINK) .c4lc_oa1.c4o .c4lc_ob1.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r > .c4lc_out_a 2>&1
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -c $(TESTS)/test_link_a.c .c4lc_oa2.c4o > /dev/null
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -c $(TESTS)/test_link_b.c .c4lc_ob2.c4o > /dev/null
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r 2>&1 | cmp - .c4lc_out_a
	$(C4RLINK) .c4lc_oa1.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r 2>&1 | cmp - .c4lc_out_a
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob1.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r 2>&1 | cmp - .c4lc_out_a
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O -c $(TESTS)/test_link_c.c .c4lc_oa2.c4o > /dev/null
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O -c $(TESTS)/test_link_d.c .c4lc_ob2.c4o > /dev/null
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-link.txt
	cat $(TESTS)/test_link_d.c $(TESTS)/test_link_c.c > .c4lc_pp.c
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp .c4lc_pp.c .c4lc_ol.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-link.txt
	# L8, extern DATA: test_link_e.c defines shared globals (scalar,
	# array, char array, struct, fn-address slot), test_link_f.c
	# declares them extern; symbol patches resolve to DATA patches.
	# Linked both orders, -O objects, and the whole-program concat
	# (extern before definition) all match committed gcc output.
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -c $(TESTS)/test_link_e.c .c4lc_oa2.c4o > /dev/null
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -c $(TESTS)/test_link_f.c .c4lc_ob2.c4o > /dev/null
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-extdata.txt
	$(C4RLINK) .c4lc_ob2.c4o .c4lc_oa2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-extdata.txt
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O -c $(TESTS)/test_link_e.c .c4lc_oa2.c4o > /dev/null
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O -c $(TESTS)/test_link_f.c .c4lc_ob2.c4o > /dev/null
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-extdata.txt
	cat $(TESTS)/test_link_f.c $(TESTS)/test_link_e.c > .c4lc_pp.c
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp .c4lc_pp.c .c4lc_ol.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-extdata.txt
	rm -f .c4lc_oa1.c4o .c4lc_ob1.c4o .c4lc_oa2.c4o .c4lc_ob2.c4o .c4lc_ol.c4r
	# L9: c4lc's own preprocessor. First the feature battery against
	# committed gcc-verified output (includes, object- and
	# function-like macros, line continuation, nesting, #ifdef/#ifndef
	# /#if/#elif/#else/#endif with constant expressions, #undef).
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -P $(TESTS)/c4lc_pp.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-pp.txt
	# Then the property that matters: preprocessing a real module
	# with c4lc instead of gcc -E must produce the SAME OBJECT, byte
	# for byte. If that holds, gcc is no longer in the pipeline.
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix src/c4ix/vfs.c .c4lc_oa2.c4o > /dev/null
	$(PREPROC) src/c4ix/vfs.c > .c4lc_pp.c
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp -O -c .c4lc_pp.c .c4lc_ob2.c4o > /dev/null
	cmp .c4lc_oa2.c4o .c4lc_ob2.c4o
	rm -f .c4lc_oa2.c4o .c4lc_ob2.c4o
	# L5, the bootstrap battery: c4lc -O compiles the interpreter it
	# runs on, and the result must run the Lisp samples (call/cc
	# exercises the CEK machine), the byte-level c4r roundtrip, and
	# c4lc's own parser.
	$(PREPROC) src/c4sp/c4sp.c > .c4lc_pp.c
	./c4sp -c 16000000 src/c4sp/lisp/c4lc.lisp -O .c4lc_pp.c .c4lc_sp.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_sp.c4r src/c4sp/lisp/fac.lisp | grep -q "Factorial of 10 = 3628800"
	./c4m load-c4r.c -- .c4lc_sp.c4r src/c4sp/lisp/truthy.lisp | cmp - src/c4sp/tests/expected/truthy.txt
	./c4m load-c4r.c -- .c4lc_sp.c4r src/c4sp/lisp/callcc.lisp | cmp - src/c4sp/tests/expected/callcc.txt
	./c4m load-c4r.c -- .c4lc_sp.c4r src/c4sp/lisp/switch.lisp | cmp - src/c4sp/tests/expected/switch.txt
	./c4m load-c4r.c -- .c4lc_sp.c4r src/c4sp/lisp/c4r-roundtrip.lisp hello.c4r | grep -q "roundtrip identical"
	./c4m load-c4r.c -- .c4lc_sp.c4r src/c4sp/lisp/c4lc-ast.lisp -check src/tests/hello.c | grep -q "parse ok"
	# L5, host independence: the same compiler must produce the same
	# bytes on three hosts -- native c4sp, c4sp.c4r under c4m, and the
	# interpreter c4lc just compiled. Compiled and compared in memory
	# (the bare VM cannot write files).
	./c4sp -c 4000000 src/c4sp/lisp/c4lc.lisp src/tests/c4lc_l2.c .c4lc_ref.c4r > /dev/null
	./c4m load-c4r.c -- c4sp.c4r -c 4000000 src/c4sp/lisp/c4lc-eq.lisp src/tests/c4lc_l2.c .c4lc_ref.c4r | grep -q "identical"
	./c4m load-c4r.c -- .c4lc_sp.c4r -c 4000000 src/c4sp/lisp/c4lc-eq.lisp src/tests/c4lc_l2.c .c4lc_ref.c4r | grep -q "identical"
	# L5, the compiler inside the OS: c4lc compiles hello.c under C4KE
	# into the kernel RAM filesystem and the kernel executes the fresh
	# image from memory -- no write ever touches the host filesystem
	cp $(TESTS)/test_ramcc.c4r .
	$(C4M) $(RUN_C4KE) test_ramcc | grep -q "yello"
	# L6, the kernel: c4lc -O compiles C4KE itself (tree passes, dead
	# function elimination, and the symbol section the trap-time
	# stacktrace needs); the result must boot cleanly and run a task
	$(MAKE) c4ke-lc.c4r
	./c4m load-c4r.c -- c4ke-lc.c4r test_basic 2>&1 | grep -q "clean shutdown"
	./c4m load-c4r.c -- c4ke-lc.c4r test_basic 2>&1 | grep -q "^  5"
	rm -f .c4lc_a.c4r .c4lc_b.c4r .c4lc_bo.c4r .c4lc_pp.c .c4lc_out_a .c4lc_sp.c4r .c4lc_ref.c4r
	@echo "test-c4lc: OK"

# The C4KE RAM filesystem: opcode-level access, the self-hosting loop
# (c4cc compiles a program inside C4KE, stores the image in the RAM
# filesystem, the kernel executes it from memory), the same loop with
# c4sp's Lisp optimizer producing the image, and c4sp reading back its
# own RAM filesystem writes.
test-c4ke-ramfs: pre c4sp.c4r
	$(C4M) $(RUN_C4KE) test_ramfs | grep -q "ramfs: ok"
	$(C4M) $(RUN_C4KE) test_selfhost | grep -q "yello"
	$(C4M) $(RUN_C4KE) test_ramopt | grep -q "yello"
	$(C4M) $(RUN_C4KE) c4sp.c4r src/c4sp/tests/ramfs.lisp | grep -q "ramfs roundtrip ok"
	@echo "test-c4ke-ramfs: OK"

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
PHONY += test-c4ix test-c4ix-fmt test-c4ix-c4 run-c4ix run-c4ix-c4 demo-c4ix demo-c4ix-c4 bench-c4ix
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
