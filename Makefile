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
# -fwrapv is a correctness flag here, not a tuning one. Everything built
# with these options emulates a machine whose arithmetic wraps -- c4m and
# c4mp are that machine, c4cc compiles for it, c4sp and c4th implement
# languages whose integers are its cells. In C, signed overflow is
# undefined, so at -O2 gcc is entitled to assume it never happens: it
# folded `if (d < 0) d = 0 - d;` on the assumption that the result must be
# positive, and SM/REM then returned the wrong quotient for a divisor of
# MIN-INT. Found by the Forth-2012 CORE suite; -O0 and -fwrapv both give
# the right answer, -O2 alone does not.
NATIVE_CC_OPTS := -O2 -fwrapv -g -idirafter include -I . $(EXTRA_CC)
NATIVE_TARGETS := c4 c4m c4cc
# Preprocessor, will use our own at some point
# We use our own include directories, and have some stdlib style headers.
# We also define the following symbols, which TODO needd to be narrowed down to a single
# definition instead of the 4 we have.
PREPROC   := gcc -E -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -C
# Our own preprocessor (src/c4dos/cpp.c), pinned byte-identical against
# gcc -E over the whole corpus by src/c4dos/tests/test-cpp.sh. Used
# wherever a build has to ANSWER a #if rather than skip it -- see the
# c4m-dos rules below.
OURCPP    := ./cpp -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1
C4        := ./c4
C4M       := ./c4m
C4CC      := ./c4cc
C4RDUMP   := ./c4rdump
C4RLINK   := ./c4rlink
# c4sp running c4lc. -R selects the recursive evaluator instead of the CEK
# machine: c4lc uses neither call/cc nor first-class environments, the two
# things CEK exists for, so it only pays the cost -- every kont push is an
# arena allocation. Worth 1.63x on the C4IX kernel build. Verified
# byte-identical on all 12 C4IX modules and on the three deep bootstrap
# images (c4ke.c, c4sp.c, c4m.c), and pinned by test-c4lc. The test rules
# below deliberately stay on the default evaluator, so both paths are
# exercised and a divergence would show up as a failing diff, not silence.
C4SPLC    := ./c4sp -R
SRCS      := src
INCLUDE   := include
C4CC_SRCS := $(SRCS)/c4cc/c4cc.c $(SRCS)/c4cc/asm-c4r.c
# Version of C4CC compiled to .c4r format
# include/c4dos.h rides along as a SOURCE file, the way u0.h does:
# c4cc has no preprocessor, and asm-c4r.c calls dos_can_write/dos_put
# to save its output when it is running as a C4DOS transient.
C4R_C4CC_SRCS := $(U0) include/c4dos.h load-c4r.c $(SRCS)/c4cc/c4cc.c $(SRCS)/c4cc/asm-c4r.c
C4KE_SRCS := load-c4r.c $(SRCS)/c4ke/c4ke.c \
             $(SRCS)/c4ke/extensions/c4ke_ipc.c $(SRCS)/c4ke/extensions/c4ke_plus.c \
             $(SRCS)/c4ke/extensions/c4ke_pm.c $(SRCS)/c4ke/extensions/c4ke_dos.c
C4KE_HDRS := $(INCLUDE)/c4.h $(INCLUDE)/c4m.h
C4KE_C4R  := c4ke.c4r
BIN_D     := $(SRCS)/c4ke/bin
C4KE_BIN  := $(BIN_D)/c4le.c4r $(BIN_D)/cat.c4r $(BIN_D)/echo.c4r \
             $(BIN_D)/kill.c4r $(BIN_D)/ls.c4r $(BIN_D)/ps.c4r \
             $(BIN_D)/spin.c4r $(BIN_D)/type.c4r $(BIN_D)/xxd.c4r \
             $(BIN_D)/b4ke.c4r
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
TESTS_C4R := $(TESTS)/raycast.c4r $(TESTS)/hello.c4r $(TESTS)/mandel.c4r $(TESTS)/factorial.c4r $(TESTS)/fun_with_ptrs.c4r \
             $(TESTS)/multifun.c4r $(TESTS)/test-order.c4r $(TESTS)/test-ptrs.c4r $(TESTS)/test_args.c4r \
			 $(TESTS)/test_basic.c4r $(TESTS)/test_crash.c4r $(TESTS)/test_customop.c4r $(TESTS)/test_exit.c4r \
			 $(TESTS)/test_fread.c4r $(TESTS)/test_infiniteloop.c4r \
			 $(TESTS)/test_malloc.c4r $(TESTS)/test_printf.c4r $(TESTS)/test_printloop.c4r \
			 $(TESTS)/test_signal.c4r $(TESTS)/test_static.c4r $(TESTS)/tests.c4r \
			 $(TESTS)/rps.c4r $(TESTS)/test_continue.c4r $(TESTS)/test_timekeeping.c4r \
			 $(TESTS)/test_float.c4r $(TESTS)/test_vprintf.c4r \
			 $(TESTS)/test_ramfs.c4r $(TESTS)/test_selfhost.c4r $(TESTS)/test_ramopt.c4r \
			 $(TESTS)/test_ramcc.c4r $(TESTS)/test_ramlink.c4r \
			 $(TESTS)/test_ixbuild.c4r $(TESTS)/cycles.c4r \
			 $(TESTS)/test_for.c4r
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
# `make test` now ASSERTS before it demonstrates.
#
# test-c4m-mem and test-c4l are real assertions with expected output,
# and both were PHONY targets in no aggregate whatsoever -- nothing ran
# them. test-c4m-mem's own comment calls its third line "the leg that
# matters most": `./c4 c4m.c load-c4r.c -- ...`, c4m interpreted by an
# UNMODIFIED c4, which is the property this whole project is about. It
# broke at fb6bf7a and stayed broken through a session and a half,
# because the test that would have said so was never invoked.
#
# A test nobody runs is a comment that costs a build step.
test: pre test-c4m-mem test-c4l
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
# c4l runs a .c4r under UNMODIFIED c4, which implements LEA..EXIT and
# nothing else -- so an image qualifies only if its compiler emitted
# nothing above EXIT. hello.c4r does; a jumptable switch does not, and
# c4l says which instruction stopped it rather than letting c4 abort
# with a bare "unknown instruction". Anything it refuses runs one
# interpreter deeper, under c4m, which is the last line here.
test-c4l: $(C4) $(C4M) $(C4CC) $(TESTS)/hello.c4r
	$(C4) c4l.c $(TESTS)/hello.c4r | grep -q yello
	$(C4CC) -o .c4l_sw.c4r $(TESTS)/test_switch.c
	$(C4) c4l.c .c4l_sw.c4r 2>&1 | grep -q "needs JMPA"
	$(C4M) load-c4r.c -- .c4l_sw.c4r | grep -q "classify(5) = 500"
	rm -f .c4l_sw.c4r
	@echo "test-c4l: OK"

# c4m's guest allocator: MALC, FREE and RALC (docs/compiler-speed.md).
# RALC was dead -- in the opcode enum and the builtin table, but with its
# VM case commented out, so guest code compiled fine and then fell through
# to the unknown-instruction path with the SIZE argument left in the
# accumulator. Using that as a pointer segfaults, which is why this test
# pins behaviour rather than merely "does not crash".
#
# What the three legs pin, and why each is here:
#   native c4m       -- the ordinary path, against a gcc-generated golden
#   ./c4 c4m.c       -- c4m interpreted by UNMODIFIED c4. This is the leg
#                       that matters most: it proves the allocator is
#                       written in the plain c4 subset, and it is the leg
#                       that caught the first design. Sizes are kept in a
#                       side table rather than a header in front of each
#                       block, because guest free() is reached by pointers
#                       guest malloc() never produced -- this chain issues
#                       8 MALCs and 16 FREEs where the native run is a
#                       balanced 17/17, so free(q - 1) would corrupt the
#                       heap. c4's own trailing "exit(N) cycle = M" line
#                       is stripped; it is c4 reporting on c4m, not output.
# The golden is gcc's, so a divergence is a real divergence from C, not
# from some earlier c4m.
test-c4m-mem: $(C4) $(C4M) $(C4CC)
	$(C4CC) -o .c4m_ralloc.c4r $(TESTS)/test_realloc.c
	$(C4M) load-c4r.c -- .c4m_ralloc.c4r | cmp - $(TESTS)/expected/test_realloc.txt
	$(C4) $(C4M).c load-c4r.c -- .c4m_ralloc.c4r | sed -e '/^exit([0-9-]*) cycle = [0-9]*$$/d' | cmp - $(TESTS)/expected/test_realloc.txt
	rm -f .c4m_ralloc.c4r
	@echo "test-c4m-mem: OK"

# cpp, the C preprocessor for the C4 toolchain (docs/c4dos-design.md).
# Strict-c4 dialect: the same source is a native binary here, a .c4r
# via c4cc, and runs interpreted under plain c4 (test-cpp checks that
# tower). It exists so the self-hosting ladder can preprocess without
# gcc; the pin is image parity against the gcc -E pipeline.
# The host build stubs the DOS API out: dos_can_write() is false, so -o
# reports that it needs a DOS rather than pretending.
cpp: src/c4dos/cpp.c include/c4dos_native.h
	gcc -O2 -idirafter include -o cpp src/c4dos/cpp.c

test-cpp: cpp $(C4) $(C4CC)
	bash src/c4dos/tests/test-cpp.sh

# C4DOS images. c4dos.c is strict-c4 dialect and must go through our
# own cpp: raw c4cc skips '#' lines entirely and would compile BOTH
# sides of the clock #if into one image. Three artifacts, because they
# answer three different questions:
#
#   c4dos.c4r        clockless, 64-bit. The PURITY pin -- it runs under
#                    unmodified ./c4 via c4l.c, which refuses any image
#                    using an opcode above EXIT, TIME included.
#   c4dos-clock.c4r  the same plus TIME, gated at runtime behind
#                    CONFIG.SYS's DEVICE=CLOCK.SYS. The dev loop.
#   c4dos32.c4r      32-bit clock build: the image c4bb boots, and the
#                    one an embedder (HOMEWARD) wants.
c4dos.c4r: cpp $(C4CC) $(SRCS)/c4dos/c4dos.c
	./cpp $(SRCS)/c4dos/c4dos.c > .c4dos_pp.c
	$(C4CC) -o $@ .c4dos_pp.c > /dev/null
	@rm -f .c4dos_pp.c

c4dos-clock.c4r: cpp $(C4CC) $(SRCS)/c4dos/c4dos.c
	./cpp -DC4DOS_CLOCK=1 $(SRCS)/c4dos/c4dos.c > .c4dos_ppck.c
	$(C4CC) -o $@ .c4dos_ppck.c > /dev/null
	@rm -f .c4dos_ppck.c

c4dos32.c4r: cpp c4cc32 $(SRCS)/c4dos/c4dos.c
	./cpp -DC4DOS_CLOCK=1 $(SRCS)/c4dos/c4dos.c > .c4dos_pp32.c
	./c4cc32 -o $@ .c4dos_pp32.c > /dev/null
	@rm -f .c4dos_pp32.c

# dostar: unpack an archive onto the RAM disk. Building C4KE in-machine
# needs its source, three extensions and two dozen headers to be THERE
# first, and a read-only disk plus a RAM disk means one archive read
# once. Format is C4TAR1 (see src/c4dos/dostar.c) -- dull on purpose,
# so the recipe below can write one with shell and strict c4 can read
# it back.
dostar.c4r: $(C4CC) include/c4dos.h $(SRCS)/c4dos/dostar.c
	$(C4CC) -o $@ include/c4dos.h $(SRCS)/c4dos/dostar.c > /dev/null
dostar32.c4r: c4cc32 include/c4dos.h $(SRCS)/c4dos/dostar.c
	./c4cc32 -o $@ include/c4dos.h $(SRCS)/c4dos/dostar.c > /dev/null

# dosload -- LOADLIN for C4DOS: loads an image, hands DOS's 4MB scratch
# back, and runs it with a command line of its own. Strict c4, nothing
# above EXIT: `make test-dosload` pins that with c4l.c, because this is
# a tool the player uses AT the C4DOS rung and it must not raise it.
dosload.c4r: $(C4CC) include/c4dos.h $(SRCS)/c4dos/dosload.c
	$(C4CC) -o $@ include/c4dos.h $(SRCS)/c4dos/dosload.c > /dev/null
dosload32.c4r: c4cc32 include/c4dos.h $(SRCS)/c4dos/dosload.c
	./c4cc32 -o $@ include/c4dos.h $(SRCS)/c4dos/dosload.c > /dev/null

# The purity pin, on its own so it can be run in a second: c4l refuses
# an image using anything above EXIT and NAMES the instruction.
test-dosload: $(C4) dosload.c4r
	@./c4 c4l.c dosload.c4r 2>&1 | grep -q "needs " && \
		{ echo "test-dosload: FAIL - dosload left the C4DOS rung:"; ./c4 c4l.c dosload.c4r; exit 1; } || true
	@echo "test-dosload: stock-c4 opcodes only OK"

# The C4KE build kit: everything cpp and c4cc need to produce a
# bootable kernel and its init process, in one file.
#
# The names in the archive are the names the SOURCES ASK FOR, not the
# repo's paths -- c4ke.c says #include "./load-c4r.c" and cpp resolves
# that against the including file's own directory, so the kernel has
# to land at the top level with load-c4r.c beside it, exactly as it
# sits in the repo root. The extensions and headers keep their paths
# because that is how they are spelled. The RAM disk is flat and its
# keys hold slashes, so a path is just a longer file name.
#
#   repo path : name in the archive
C4KE_KIT := src/c4ke/c4ke.c:c4ke.c \
            load-c4r.c:load-c4r.c \
            src/c4ke/extensions/c4ke_ipc.c:src/c4ke/extensions/c4ke_ipc.c \
            src/c4ke/extensions/c4ke_plus.c:src/c4ke/extensions/c4ke_plus.c \
            src/c4ke/extensions/c4ke_pm.c:src/c4ke/extensions/c4ke_pm.c \
            src/c4ke/extensions/c4ke_dos.c:src/c4ke/extensions/c4ke_dos.c \
            src/c4ke/bin/ps.c:ps.c \
            src/c4ke/bin/eshell.c:eshell.c \
            src/c4ke/services/init.c:init.c \
            $(foreach h,$(wildcard include/*.h),$(h):$(h)) \
            $(foreach h,$(wildcard include/c4ke/*.h),$(h):$(h))

# $(1) = output archive, $(2) = src:name pairs
define c4tar_build
	@printf 'C4TAR1\n' > $(1)
	@for pair in $(2); do \
		src=$${pair%%:*}; name=$${pair#*:}; \
		printf '%s %d\n' "$$name" "$$(wc -c < $$src)" >> $(1); \
		cat $$src >> $(1); \
	done
	@echo "$(1): $$(wc -c < $(1)) bytes, $$(echo $(2) | wc -w) files"
endef

# prerequisites are the SOURCE halves of the pairs, not the names
# The compiler's own source, so the machine can rebuild the toolchain
# before it builds anything else -- LADDER.BAT, docs/compiler-on-the-board.md.
# Flat names: c4cc has no preprocessor and skips '#' lines, so nothing
# here needs its directory back.
# u0lite.h rides along because it is what a program on this rung is
# compiled AGAINST -- rebuilding mandel or rps in the machine means
# handing c4cc that file instead of u0.h, and it has to be there to
# hand over. See include/u0lite.h.
C4TOOLS_KIT := include/c4dos.h:c4dos.h load-c4r.c:load-c4r.c \
               include/u0lite.h:u0lite.h \
               $(SRCS)/c4cc/c4cc.c:c4cc.c $(SRCS)/c4cc/asm-c4r.c:asm-c4r.c \
               $(SRCS)/c4dos/cpp.c:cpp.c
C4TOOLS_KIT_SRCS := $(foreach p,$(C4TOOLS_KIT),$(firstword $(subst :, ,$(p))))
tools-src.tar: $(C4TOOLS_KIT_SRCS)
	$(call c4tar_build,tools-src.tar,$(C4TOOLS_KIT))

C4KE_KIT_SRCS := $(foreach p,$(C4KE_KIT),$(firstword $(subst :, ,$(p))))
# src/c4ix spelled out rather than $(C4IX_SRC): both it and C4IX_MODS
# are defined further down, and := expands now. The Makefile already
# carries one bug of exactly this shape (see c4rlink.c's note on $(U0)).
C4IX_KIT := $(foreach m,boot console va host sl4b task sched vfs sys c4ke loader init,\
              src/c4ix/$(m).c:src/c4ix/$(m).c) \
            src/c4ix/c4ix.h:src/c4ix/c4ix.h \
            $(foreach h,$(wildcard include/*.h),$(h):$(h)) \
            $(foreach h,$(wildcard include/c4ke/*.h),$(h):$(h))
C4IX_KIT_SRCS := $(foreach p,$(C4IX_KIT),$(firstword $(subst :, ,$(p))))
c4ix-src.tar: $(C4IX_KIT_SRCS)
	$(call c4tar_build,c4ix-src.tar,$(C4IX_KIT))

c4ke-src.tar: $(C4KE_KIT_SRCS)
	$(call c4tar_build,c4ke-src.tar,$(C4KE_KIT))

# raycast's C4DOS build# raycast's C4DOS build: stock-c4 opcodes plus TIME, nothing else. A
# transient with something to draw, and the honest test of that
# restriction (src/tests/raycast.c's RC_DOS branch).
raycast-dos.c4r: c4sp $(C4LC_LISP) $(TESTS)/raycast.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O -conforming -D RC_DOS=1 \
		$(TESTS)/raycast.c $@ > /dev/null

# A boot floppy for the native run targets. C4DOS has no notion of a
# drive: it opens config.sys, autoexec.bat and c4dos.dir relative to
# the working directory, so "the disk" is simply a directory you cd
# into. c4dos.dir IS the directory service -- DIR types that file,
# because the raw disk cannot enumerate itself -- so it is generated
# from whatever actually landed here.
C4DOS_DISK := c4dos-disk
$(C4DOS_DISK): c4dos-clock.c4r $(TESTS)/hello.c4r raycast-dos.c4r \
               $(SRCS)/c4dos/fs/CONFIG.SYS $(SRCS)/c4dos/fs/AUTOEXEC.BAT
	@mkdir -p $(C4DOS_DISK)
	@cp $(SRCS)/c4dos/fs/CONFIG.SYS   $(C4DOS_DISK)/config.sys
	@cp $(SRCS)/c4dos/fs/AUTOEXEC.BAT $(C4DOS_DISK)/autoexec.bat
	@cp $(TESTS)/hello.c4r            $(C4DOS_DISK)/hello.c4r
	@cp raycast-dos.c4r               $(C4DOS_DISK)/raycast.c4r
	@# The redirection creates c4dos.dir before ls runs, so the listing
	@# includes itself, which is what DOS expects to see. Real case, not
	@# shouted: this listing is also the NAME RESOLVER -- dos_open opens
	@# whatever spelling it finds here -- so an entry of HELLO.C4R beside
	@# a file called hello.c4r would defeat the lookup it exists to serve.
	@cd $(C4DOS_DISK) && ls > c4dos.dir
	@echo "c4dos-disk: ready -- try DIR, VER, TIME, RUN hello.c4r, RUN raycast.c4r"

# The 32-bit halves of the same three things. c4bb is a 32-bit machine
# and refuses a 64-bit image outright, so its floppy needs its own
# transients rather than the ones next door.
hello32.c4r: c4cc32 $(TESTS)/hello.c
	./c4cc32 -o $@ $(TESTS)/hello.c > /dev/null

raycast-dos32.c4r: c4sp32 $(C4LC_LISP) $(TESTS)/raycast.c
	./c4sp32 src/c4sp/lisp/c4lc.lisp -O -conforming -D RC_DOS=1 \
		$(TESTS)/raycast.c $@ > /dev/null

# The rest of the C4DOS rung: the same sources the C4KE builds use,
# compiled against u0lite instead of u0 (include/u0lite.h says why).
# No preprocessor is involved and no -D is passed -- what makes these
# base-c4 images is which library they are handed, and c4cc has no # to
# read anyway.
#
# c4.c and c4m.c are handed NOTHING, and that is the interesting part.
# Both are self-contained C, and unpreprocessed c4cc walks straight into
# c4m.c's `#if C4_ONLY` branch -- the one written for running under
# plain c4 -- because it skips the # lines and compiles the code
# between them. So the VM's own C4DOS build falls out of the source as
# it stands, with no flag and no second file: `c4m.c4r` inside
# `c4.c4r` inside the board is the whole VM proved in software, needing
# not one opcode the machine did not have on the day it booted.
# The 64-bit twins of the same four. The native build floppy wants them
# for exactly the reason the 32-bit one does: an image compiled against
# u0lite (or against nothing, for c4.c and c4m.c) needs no opcode above
# EXIT, so ONE file runs both at the A> prompt and inside the C4KE that
# C4DOS builds. A u0 build would only run in the kernel.
c4-dos.c4r: $(C4CC) c4.c
	$(C4CC) -o $@ c4.c > /dev/null

# c4m for the C4DOS rung, through OUR OWN preprocessor.
#
# It used to go through raw c4cc, and relied on c4cc skipping '#' lines
# to switch the #if C4M_DOS branch on. That works, and it is also how
# the shared disk's c4m ended up with no u0.h, no c4.h and no
# c4m_float.h and nobody noticing for a round -- c4cc skips '#include'
# too (docs/dos-rung-fixes.md round four). A build should ask for what
# it wants:
#   -DC4M_FREESTANDING=1  no headers, the way c4cc used to get by never
#                   opening one. The census insists: the tree's own
#                   <string.h> defines memmove() via memcpy(), and MCPY
#                   is opcode 42, above this image's rung.
#   -DC4_ONLY=1     the C4 implementations, since there is no host libc
#   -DNOT_NATIVE=1  what c4.h would have derived from __c4cc__
#   -DC4M_DOS=1     the DOS file API and the DOS clock
#   -DC4M_NO_U0=1   u0 declines to run under C4DOS by design (F5)
# Same opcode set as the old raw-c4cc image, 63 bytes smaller (the dead
# `if (0)` arm of C4IV goes), and now the result of a decision.
# Still base-c4: c4dos.h reaches the API through its invoke STUB
# (a self-rewritten JMP), not an indirect call, and it rides along as a
# source file because that is what its own header says it is.
# c4mpg -- c4m with the memory protection guard. docs/c4mpg-design.md.
#
# A SEPARATE BINARY, never a runtime flag: the guard costs time on every
# load and store, and c4m is what the whole ladder is benchmarked on
# (docs/compiler-speed.md). A guard you can leave on by accident quietly
# changes every measurement in the repo.
#
# A FORK of c4m.c, not one source with an #ifdef. The first attempt was
# the latter and it was wrong for a reason worth keeping: c4m.c is read
# as SOURCE at run time, by a plain c4 that has no preprocessor and
# compiles what is between the '#' lines it skips. innerbench does
# exactly that -- `c4 c4m.c load-c4r.c -- src/c4ke/c4ke.c` -- so the
# guard would have been unconditional code in every nested interpreter
# on the board. Checking that no BUILD used raw c4cc was true and beside
# the point; a conditional is only a conditional to something that
# evaluates it.
#
# A fork rots, so it is pinned: src/tests/mpg/check-fork.sh strips the
# marked blocks out of c4mpg.c and demands byte-identity with c4m.c.
# make test-mpg runs it first.
# Both halves of the guard: the known-bad set is caught AND named, and
# the whole corpus still runs identically under it. The second half is
# the one that keeps it usable -- docs/c4mpg-design.md.
test-mpg: c4mpg c4m
	bash src/tests/mpg/check-fork.sh
	bash src/tests/mpg/test-mpg.sh

c4mpg: c4mpg.c c4m_float.c
	gcc $(NATIVE_CC_OPTS) c4mpg.c c4m_float.c -o $@ -lm

c4mpg32: c4mpg.c c4m_float.c
	gcc -m32 $(NATIVE_CC_OPTS) c4mpg.c c4m_float.c -o $@ -lm

c4m-dos.c4r: $(C4CC) cpp $(INCLUDE)/c4dos.h c4m.c
	$(OURCPP) -DC4M_FREESTANDING=1 -DC4_ONLY=1 -DNOT_NATIVE=1 -DC4M_DOS=1 -DC4M_NO_U0=1 $(INCLUDE)/c4dos.h c4m.c > .c4m_dos_pp.c
	$(C4CC) -o $@ .c4m_dos_pp.c > /dev/null
	@rm -f .c4m_dos_pp.c

mandel-dos.c4r: $(C4CC) $(INCLUDE)/u0lite.h $(TESTS)/mandel.c
	$(C4CC) -o $@ $(INCLUDE)/u0lite.h $(TESTS)/mandel.c > /dev/null

rps-dos.c4r: $(C4CC) $(INCLUDE)/u0lite.h $(TESTS)/rps.c
	$(C4CC) -o $@ $(INCLUDE)/u0lite.h $(TESTS)/rps.c > /dev/null

c4-dos32.c4r: c4cc32 c4.c
	./c4cc32 -o $@ c4.c > /dev/null

# 32-bit twin of c4m-dos.c4r above, same three -D and the same reason.
c4m-dos32.c4r: c4cc32 cpp $(INCLUDE)/c4dos.h c4m.c
	$(OURCPP) -DC4M_FREESTANDING=1 -DC4_ONLY=1 -DNOT_NATIVE=1 -DC4M_DOS=1 -DC4M_NO_U0=1 $(INCLUDE)/c4dos.h c4m.c > .c4m_dos32_pp.c
	./c4cc32 -o $@ .c4m_dos32_pp.c > /dev/null
	@rm -f .c4m_dos32_pp.c

mandel-dos32.c4r: c4cc32 $(INCLUDE)/u0lite.h $(TESTS)/mandel.c
	./c4cc32 -o $@ $(INCLUDE)/u0lite.h $(TESTS)/mandel.c > /dev/null

rps-dos32.c4r: c4cc32 $(INCLUDE)/u0lite.h $(TESTS)/rps.c
	./c4cc32 -o $@ $(INCLUDE)/u0lite.h $(TESTS)/rps.c > /dev/null

C4DOS_DISK32 := c4dos-disk32
$(C4DOS_DISK32): hello32.c4r raycast-dos32.c4r \
                 $(SRCS)/c4dos/fs/CONFIG.SYS $(SRCS)/c4dos/fs/AUTOEXEC.BAT
	@mkdir -p $(C4DOS_DISK32)
	@cp $(SRCS)/c4dos/fs/CONFIG.SYS   $(C4DOS_DISK32)/config.sys
	@# c4bb's UART emits each byte as it is written, so the classic
	@# tight A> prompt works here. Say so: DOS defaults to a prompt on
	@# its own line, because a host libc buffers a partial one.
	@echo 'DEVICE=CONSOLE.SYS FLUSH' >> $(C4DOS_DISK32)/config.sys
	@# And the drives. Same rule: the board has them at 0x13c/0x188,
	@# native c4m has ordinary memory there, so DOS is told rather than
	@# left to probe. Without this line the prompt is A> and stays A>.
	@echo 'DEVICE=DRIVES.SYS' >> $(C4DOS_DISK32)/config.sys
	@cp $(SRCS)/c4dos/fs/AUTOEXEC.BAT $(C4DOS_DISK32)/autoexec.bat
	@cp hello32.c4r                   $(C4DOS_DISK32)/hello.c4r
	@cp raycast-dos32.c4r             $(C4DOS_DISK32)/raycast.c4r
	@cd $(C4DOS_DISK32) && ls > c4dos.dir
	@echo "c4dos-disk32: ready (32-bit, for c4bb)"

# A floppy that can BUILD C4KE and boot it, which is the whole ladder
# in one directory: dostar unpacks the sources onto the RAM disk, cpp
# and c4cc turn them into a kernel, and RUN boots it.
#
# init.c4r/c4sh.c4r/vfsload.c4r ride along prebuilt. BUILD.BAT does
# compile init itself, but the kernel's own loader (load-c4r.c) opens
# files with the host open() and has never heard of the DOS RAM disk,
# so the copy it can actually LOAD is the one on the disk. Building it
# in-machine and booting that same image needs load-c4r.c to learn the
# API -- see docs/c4dos-design.md.
# vfsload, 64-bit. It is what gives a booted C4KE a populated ramfs --
# ls lists the RAM filesystem, not the host directory -- and until now
# it only ever got built at 32 bits, by src/c4bb/tests/build-images.sh.
# Same recipe as that script: c4lc rather than c4cc (vfsload wants real
# block scoping, not c4cc's top-of-function declarations), and gcc -E
# first because c4lc's own preprocessor hangs on u0.h -- a pre-existing
# issue that reproduces at 64 bits too.
vfsload.c4r: c4sp $(C4LC_LISP) $(BIN_D)/vfsload.c $(U0)
	$(PREPROC) $(BIN_D)/vfsload.c > .vfsload_pp.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O .vfsload_pp.c $@ > /dev/null
	@rm -f .vfsload_pp.c

C4DOS_BUILD_DISK := c4dos-build
$(C4DOS_BUILD_DISK): c4dos-clock.c4r dostar.c4r cpp.c4r c4cc.c4r c4ke-src.tar dosload.c4r \
                     tools-src.tar $(SRCS)/c4dos/fs/LADDER.BAT \
                     $(INIT) $(C4SH) $(VFS) $(SRCS)/c4dos/fs/BUILD.BAT \
                     $(C4KE_BIN) $(C4R_TOP) $(BENCHS) vfsload.c4r \
                     tar.c4r $(C4R_C4RLINK) \
                     c4-dos.c4r c4m-dos.c4r mandel-dos.c4r rps-dos.c4r raycast-dos.c4r \
                     $(SRCS)/c4bb/fs/c4ke-build.vfs.txt
	@mkdir -p $(C4DOS_BUILD_DISK)
	@sed 's/SIZE=[0-9]*/SIZE=16777216/' $(SRCS)/c4dos/fs/CONFIG.SYS > $(C4DOS_BUILD_DISK)/config.sys
	@cp $(SRCS)/c4dos/fs/AUTOEXEC.BAT $(C4DOS_BUILD_DISK)/autoexec.bat
	@cp $(SRCS)/c4dos/fs/BUILD.BAT    $(C4DOS_BUILD_DISK)/build.bat
	@cp $(SRCS)/c4dos/fs/LADDER.BAT   $(C4DOS_BUILD_DISK)/ladder.bat
	@cp dostar.c4r cpp.c4r c4cc.c4r c4ke-src.tar dosload.c4r tools-src.tar $(C4DOS_BUILD_DISK)/
	@cp $(INIT) $(C4SH) $(VFS) $(C4DOS_BUILD_DISK)/
	@# C4KE's userland. Without it the kernel this floppy builds comes
	@# up with a shell and nothing to run in it -- `ls` answered
	@# "unable to open 'ls' or 'ls.c4r'", which is what this whole
	@# section is here to fix. Nothing needs to change in the kernel:
	@# task_loadc4r checks the ramfs and then the HOST, and the run
	@# target cd's into this directory, so a .c4r sitting here is
	@# loadable. Copied from where they are built, not from the repo
	@# root, whose copies only exist once `make pre` has run.
	@cp $(C4KE_BIN) $(C4DOS_BUILD_DISK)/
	@cp $(C4R_TOP) $(BENCHS) $(C4DOS_BUILD_DISK)/
	@# vfsload is what gives the booted kernel a populated tree rather
	@# than a bare root: `ls` lists the RAM filesystem, not this
	@# directory. Its manifest is the floppy's own -- the repo-root one
	@# names two dozen files that are not here and would print a column
	@# of "cannot open" over the boot.
	@cp vfsload.c4r $(C4DOS_BUILD_DISK)/
	@# tar and c4rlink: with these, the kernel this floppy builds can
	@# unpack its own sources and link objects, which is the shape the
	@# rung above needs. b4ke rides along in $(C4KE_BIN) and drives them.
	@cp tar.c4r $(C4R_C4RLINK) $(C4DOS_BUILD_DISK)/
	@cp $(SRCS)/c4bb/fs/c4ke-build.vfs.txt $(C4DOS_BUILD_DISK)/c4ke.vfs.txt
	@# The C4DOS rung proper: mandel, rps and raycast, plus c4 and c4m,
	@# all compiled against u0lite or against nothing at all. That is
	@# what makes ONE image run both at the A> prompt and inside the
	@# kernel the machine just built -- see include/u0lite.h. (mandel
	@# and raycast want TIME, to time themselves; this floppy boots the
	@# clock build, so they have it.)
	@cp mandel-dos.c4r  $(C4DOS_BUILD_DISK)/mandel.c4r
	@cp rps-dos.c4r     $(C4DOS_BUILD_DISK)/rps.c4r
	@cp raycast-dos.c4r $(C4DOS_BUILD_DISK)/raycast.c4r
	@cp c4-dos.c4r      $(C4DOS_BUILD_DISK)/c4.c4r
	@cp c4m-dos.c4r     $(C4DOS_BUILD_DISK)/c4m.c4r
	@# innerbench compiles a whole C4KE inside a nested c4m, so it wants
	@# these by the exact names it opens them with (src/bench/innerbench.c).
	@mkdir -p $(C4DOS_BUILD_DISK)/src/c4ke
	@cp $(SRCS)/c4ke/c4ke.c $(C4DOS_BUILD_DISK)/src/c4ke/c4ke.c
	@cp load-c4r.c c4m.c c4.c $(C4DOS_BUILD_DISK)/
	@cd $(C4DOS_BUILD_DISK) && ls -p | grep -v '/$$' > c4dos.dir
	@echo "c4dos-build: ready -- boot it, type BUILD, then RUN dosload.c4r c4ke.c4r"

# cpp as a 64-bit image, for the native build floppy
cpp.c4r: $(C4CC) include/c4dos.h $(SRCS)/c4dos/cpp.c
	$(C4CC) -o $@ include/c4dos.h $(SRCS)/c4dos/cpp.c > /dev/null

# The ladder, interactively: boot C4DOS on that floppy and type BUILD.
run-c4dos-build: $(C4M) c4dos-clock.c4r $(C4DOS_BUILD_DISK)
	@cd $(C4DOS_BUILD_DISK) && $(CURDIR)/c4m $(CURDIR)/load-c4r.c -- $(CURDIR)/c4dos-clock.c4r

# The same floppy at 32 bits, which is the one the BREADBOARD boots. Every
# image on it has to be one c4bb can load, so all six come from c4cc32.
# docs/compiler-on-the-board.md is the tracker; the point of this disk is
# that the machine builds its own kernel in seconds rather than in an hour.
C4DOS_BUILD_DISK32 := c4dos-build32
$(C4DOS_BUILD_DISK32): c4dos32.c4r dostar32.c4r cpp32.c4r c4cc32.c4r c4ke-src.tar \
                       dosload32.c4r init32.c4r c4sh32.c4r c4ke.vfs32.c4r \
                       tools-src.tar $(SRCS)/c4dos/fs/LADDER.BAT \
                       $(SRCS)/c4dos/fs/BUILD.BAT ls32.c4r ps32.c4r \
                       c4-dos32.c4r c4m-dos32.c4r mandel-dos32.c4r rps-dos32.c4r \
                       raycast-dos32.c4r $(SRCS)/c4bb/fs/c4ke-build.vfs.txt \
                       tar32.c4r c4rlink32.c4r b4ke32.c4r
	@mkdir -p $(C4DOS_BUILD_DISK32)
	@sed 's/SIZE=[0-9]*/SIZE=16777216/' $(SRCS)/c4dos/fs/CONFIG.SYS > $(C4DOS_BUILD_DISK32)/config.sys
	@# c4bb's UART emits each byte as it is written, so the classic
	@# tight A> prompt works here. Say so: DOS defaults to a prompt on
	@# its own line, because a host libc buffers a partial one.
	@echo 'DEVICE=CONSOLE.SYS FLUSH' >> $(C4DOS_BUILD_DISK32)/config.sys
	@# And the drives. Same rule: the board has them at 0x13c/0x188,
	@# native c4m has ordinary memory there, so DOS is told rather than
	@# left to probe. Without this line the prompt is A> and stays A>.
	@echo 'DEVICE=DRIVES.SYS' >> $(C4DOS_BUILD_DISK32)/config.sys
	@cp $(SRCS)/c4dos/fs/AUTOEXEC.BAT $(C4DOS_BUILD_DISK32)/autoexec.bat
	@cp $(SRCS)/c4dos/fs/BUILD.BAT    $(C4DOS_BUILD_DISK32)/build.bat
	@cp $(SRCS)/c4dos/fs/LADDER.BAT   $(C4DOS_BUILD_DISK32)/ladder.bat
	@cp c4ke-src.tar tools-src.tar    $(C4DOS_BUILD_DISK32)/
	@cp dostar32.c4r   $(C4DOS_BUILD_DISK32)/dostar.c4r
	@cp cpp32.c4r      $(C4DOS_BUILD_DISK32)/cpp.c4r
	@cp c4cc32.c4r     $(C4DOS_BUILD_DISK32)/c4cc.c4r
	@cp dosload32.c4r  $(C4DOS_BUILD_DISK32)/dosload.c4r
	@cp init32.c4r     $(C4DOS_BUILD_DISK32)/init.c4r
	@cp c4sh32.c4r     $(C4DOS_BUILD_DISK32)/c4sh.c4r
	@cp c4ke.vfs32.c4r $(C4DOS_BUILD_DISK32)/c4ke.vfs.c4r
	@# The same userland the 64-bit build floppy carries, at 32 bits.
	@# Built here rather than copied, because the disk that has these
	@# prebuilt is derived from this family and the dependency would be
	@# a circle -- the same reasoning as $(C4DOS_IX_DISK) above.
	@cp b4ke32.c4r    $(C4DOS_BUILD_DISK32)/b4ke.c4r
	@cp tar32.c4r     $(C4DOS_BUILD_DISK32)/tar.c4r
	@cp c4rlink32.c4r $(C4DOS_BUILD_DISK32)/c4rlink.c4r
	@cp ls32.c4r $(C4DOS_BUILD_DISK32)/ls.c4r
	@cp ps32.c4r $(C4DOS_BUILD_DISK32)/ps.c4r
	@for t in cat echo kill spin type xxd c4le; do \
	   ./c4cc32 -o $(C4DOS_BUILD_DISK32)/$$t.c4r $(U0) $(BIN_D)/$$t.c > /dev/null || exit 1; \
	 done
	@./c4cc32 -o $(C4DOS_BUILD_DISK32)/top.c4r $(U0) $(BIN_D)/ps.c $(BIN_D)/top.c > /dev/null
	@./c4cc32 -o $(C4DOS_BUILD_DISK32)/bench.c4r $(U0) $(SRCS)/bench/bench.c > /dev/null
	@./c4cc32 -o $(C4DOS_BUILD_DISK32)/benchtop.c4r $(U0) $(BIN_D)/ps.c $(SRCS)/bench/benchtop.c > /dev/null
	@./c4cc32 -o $(C4DOS_BUILD_DISK32)/innerbench.c4r $(U0) $(SRCS)/bench/innerbench.c > /dev/null
	@# The C4DOS rung: u0lite (or bare) builds, so one image runs both
	@# at the A> prompt and inside the kernel BUILD produces.
	@cp mandel-dos32.c4r  $(C4DOS_BUILD_DISK32)/mandel.c4r
	@cp rps-dos32.c4r     $(C4DOS_BUILD_DISK32)/rps.c4r
	@cp raycast-dos32.c4r $(C4DOS_BUILD_DISK32)/raycast.c4r
	@cp c4-dos32.c4r      $(C4DOS_BUILD_DISK32)/c4.c4r
	@cp c4m-dos32.c4r     $(C4DOS_BUILD_DISK32)/c4m.c4r
	@# innerbench opens these by the exact names in src/bench/innerbench.c.
	@mkdir -p $(C4DOS_BUILD_DISK32)/src/c4ke
	@cp $(SRCS)/c4ke/c4ke.c $(C4DOS_BUILD_DISK32)/src/c4ke/c4ke.c
	@cp load-c4r.c c4m.c c4.c $(C4DOS_BUILD_DISK32)/
	@# vfsload gives the booted kernel a populated tree. The 32-bit one
	@# comes from build-images.sh (it needs c4lc), so take it if it is
	@# there -- the floppy is still usable without it.
	@cp $(SRCS)/c4bb/images/disk/vfsload.c4r $(C4DOS_BUILD_DISK32)/ 2>/dev/null || true
	@cp $(SRCS)/c4bb/fs/c4ke-build.vfs.txt $(C4DOS_BUILD_DISK32)/c4ke.vfs.txt
	@cd $(C4DOS_BUILD_DISK32) && ls -p | grep -v '/$$' > c4dos.dir
	@echo "c4dos-build32: ready -- boot it on c4bb, type BUILD"

# The whole climb on one floppy: C4DOS boots, LADDER builds C4KE from
# source, IX builds C4IX with the compiled compiler, and dosload boots
# either one. docs/compiler-on-the-board.md walks it.
C4DOS_IX_DISK := c4dos-c4ix32
$(C4DOS_IX_DISK): c4dos32.c4r dostar32.c4r cpp32.c4r c4cc32.c4r dosload32.c4r \
                  c4rlink32.c4r c4sc32-fused.c4r c4ke-src.tar c4ix-src.tar \
                  tools-src.tar init32.c4r c4sh32.c4r c4ke.vfs32.c4r \
                  b4ke32.c4r tar32.c4r ls32.c4r ps32.c4r \
                  bbsave32.c4r save32.c4r raycast-dos32.c4r reboot32.c4r \
                  install32.c4r $(SRCS)/c4dos/fs/install.lst \
                  kinstall32.c4r $(SRCS)/c4ke/fs/c4ix.lst \
                  $(SRCS)/c4bb/fs/c4ke-climb.vfs.txt \
                  $(SRCS)/c4bb/fs/c4ix-climb.vfs.txt \
                  c4-dos32.c4r c4m-dos32.c4r mandel-dos32.c4r rps-dos32.c4r \
                  $(BIN_D)/c4ix.b4k \
                  $(SRCS)/c4dos/fs/LADDER.BAT $(SRCS)/c4dos/fs/IX.BAT \
                  $(SRCS)/c4dos/fs/c4ix.objs $(SRCS)/c4dos/fs/CONFIG.SYS
	@mkdir -p $(C4DOS_IX_DISK)
	@sed 's/SIZE=[0-9]*/SIZE=33554432/' $(SRCS)/c4dos/fs/CONFIG.SYS > $(C4DOS_IX_DISK)/config.sys
	@# c4bb's UART emits each byte as it is written, so the classic
	@# tight A> prompt works here. Say so: DOS defaults to a prompt on
	@# its own line, because a host libc buffers a partial one.
	@echo 'DEVICE=CONSOLE.SYS FLUSH' >> $(C4DOS_IX_DISK)/config.sys
	@# And the drives. Same rule: the board has them at 0x13c/0x188,
	@# native c4m has ordinary memory there, so DOS is told rather than
	@# left to probe. Without this line the prompt is A> and stays A>.
	@echo 'DEVICE=DRIVES.SYS' >> $(C4DOS_IX_DISK)/config.sys
	@cp $(SRCS)/c4dos/fs/AUTOEXEC.BAT $(C4DOS_IX_DISK)/autoexec.bat
	@cp $(SRCS)/c4dos/fs/LADDER.BAT   $(C4DOS_IX_DISK)/ladder.bat
	@cp $(SRCS)/c4dos/fs/IX.BAT       $(C4DOS_IX_DISK)/ix.bat
	@cp $(SRCS)/c4dos/fs/c4ix.objs    $(C4DOS_IX_DISK)/
	@cp c4ke-src.tar c4ix-src.tar tools-src.tar $(C4DOS_IX_DISK)/
	@cp dostar32.c4r   $(C4DOS_IX_DISK)/dostar.c4r
	@cp cpp32.c4r      $(C4DOS_IX_DISK)/cpp.c4r
	@cp c4cc32.c4r     $(C4DOS_IX_DISK)/c4cc.c4r
	@cp dosload32.c4r  $(C4DOS_IX_DISK)/dosload.c4r
	@cp c4rlink32.c4r  $(C4DOS_IX_DISK)/c4rlink.c4r
	@cp c4sc32-fused.c4r $(C4DOS_IX_DISK)/c4sc.c4r
	@cp init32.c4r     $(C4DOS_IX_DISK)/init.c4r
	@cp c4sh32.c4r     $(C4DOS_IX_DISK)/c4sh.c4r
	@cp c4ke.vfs32.c4r $(C4DOS_IX_DISK)/c4ke.vfs.c4r
	@# The C4KE rung. Everything above this line is what C4DOS needs to
	@# build and boot a kernel; these four are what that kernel needs to
	@# build C4IX ITSELF -- tar unpacks the source into its own RAM
	@# filesystem, b4ke drives the twelve compiles and the link, and ls
	@# and ps are there so the shell has something to show.
	@cp b4ke32.c4r     $(C4DOS_IX_DISK)/b4ke.c4r
	@cp tar32.c4r      $(C4DOS_IX_DISK)/tar.c4r
	@cp ls32.c4r       $(C4DOS_IX_DISK)/ls.c4r
	@cp ps32.c4r       $(C4DOS_IX_DISK)/ps.c4r
	@cp $(BIN_D)/c4ix.b4k $(C4DOS_IX_DISK)/
	@cp reboot32.c4r   $(C4DOS_IX_DISK)/reboot.c4r
	@# What makes this medium bootable to the BIOS (docs/c4bb-storage.md
	@# M10). A name rather than a second copy of the image: the disk
	@# already carries c4dos32.c4r's kernel under its own name.
	@cp c4dos32.c4r    $(C4DOS_IX_DISK)/
	@echo c4dos32.c4r > $(C4DOS_IX_DISK)/boot.cfg
	@# Keeping what you built: bbsave writes C4DOS's RAM disk onto
	@# another drive, save does the same for C4KE's ramfs. Both need a
	@# writable medium in the machine -- `cli.js -w dir`.
	@cp bbsave32.c4r   $(C4DOS_IX_DISK)/bbsave.c4r
	@cp save32.c4r     $(C4DOS_IX_DISK)/save.c4r
	@# And the one that makes a medium BOOTABLE rather than merely
	@# written: install reads install.lst and fetches every name from
	@# wherever it is -- the RAM disk for what LADDER just built, this
	@# floppy for everything else -- then writes boot.cfg. M11.
	@cp install32.c4r  $(C4DOS_IX_DISK)/install.c4r
	@cp $(SRCS)/c4dos/fs/install.lst $(C4DOS_IX_DISK)/install.lst
	@# And its counterpart one rung up, which install.lst carries onto
	@# the C4KE medium so that the system C4KE builds can be installed
	@# the same way. M12.
	@cp kinstall32.c4r $(C4DOS_IX_DISK)/kinstall.c4r
	@cp $(SRCS)/c4ke/fs/c4ix.lst     $(C4DOS_IX_DISK)/c4ix.lst
	@# C4KE's own userland. It was on c4ke-root and on none of the disks
	@# a player ever boots, so the kernel they built came up with a
	@# shell and almost nothing to run in it. Built here rather than
	@# copied from another disk, because that disk is derived from this
	@# one and the dependency would be a circle.
	@for t in cat echo kill spin type xxd c4le; do \
	   ./c4cc32 -o $(C4DOS_IX_DISK)/$$t.c4r $(U0) $(BIN_D)/$$t.c > /dev/null || exit 1; \
	 done
	@./c4cc32 -o $(C4DOS_IX_DISK)/top.c4r $(U0) $(BIN_D)/ps.c $(BIN_D)/top.c > /dev/null
	@./c4cc32 -o $(C4DOS_IX_DISK)/bench.c4r $(U0) $(SRCS)/bench/bench.c > /dev/null
	@./c4cc32 -o $(C4DOS_IX_DISK)/benchtop.c4r $(U0) $(BIN_D)/ps.c $(SRCS)/bench/benchtop.c > /dev/null
	@./c4cc32 -o $(C4DOS_IX_DISK)/innerbench.c4r $(U0) $(SRCS)/bench/innerbench.c > /dev/null
	@# The C4DOS rung proper: mandel, rps, raycast, c4 and c4m, none of
	@# them needing an opcode above EXIT (raycast and mandel want TIME,
	@# to time themselves). These are what a player has to play with
	@# BEFORE extending the CPU, which is the whole reason they are
	@# built this way -- see include/u0lite.h and test-c4bb-baseops.
	@cp mandel-dos32.c4r $(C4DOS_IX_DISK)/mandel.c4r
	@cp rps-dos32.c4r    $(C4DOS_IX_DISK)/rps.c4r
	@# raycast needs c4lc: its enums have expressions in them, which is
	@# an L11 feature c4cc does not have. The DOS build is stock-c4
	@# opcodes only, so it runs on every rung from here up.
	@cp raycast-dos32.c4r $(C4DOS_IX_DISK)/raycast.c4r
	@# c4 running c4m running a .c4r is the VM proved in software, and
	@# it has to be provable at THIS rung or it proves nothing about
	@# what the machine can already do. Both come from the unadorned
	@# sources; the u0 and preprocessed builds are the ones one rung up.
	@cp c4-dos32.c4r  $(C4DOS_IX_DISK)/c4.c4r
	@cp c4m-dos32.c4r $(C4DOS_IX_DISK)/c4m.c4r
	@# innerbench compiles a whole C4KE inside a nested c4m, so it wants
	@# these three by the exact names it opens them with.
	@mkdir -p $(C4DOS_IX_DISK)/src/c4ke
	@cp src/c4ke/c4ke.c $(C4DOS_IX_DISK)/src/c4ke/c4ke.c
	@cp load-c4r.c c4m.c $(C4DOS_IX_DISK)/
	@# C4IX's own userland, prebuilt at 32 bits by build-images.sh: init
	@# looks for c4ix-sh.c4r and the kernel it just built has no way to
	@# make one, because libc4ix is a separate ladder.
	@# All of it, not the four it used to be: 417 KB total, and the
	@# medium C4KE installs one rung up is only a real system if the
	@# shell has something to run. c4ix-vfsload is what init spawns
	@# before the shell, so without it C4IX boots to a bare tree.
	@cp src/c4bb/images/disk/c4ix-*.c4r $(C4DOS_IX_DISK)/ 2>/dev/null || true
	@cp $(SRCS)/c4bb/fs/c4ix-climb.vfs.txt $(C4DOS_IX_DISK)/c4ix.vfs.txt
	@# vfsload is what gives a booted C4KE a source tree rather than a
	@# bare root. It needs c4lc (real block scoping, not c4cc's), so it
	@# comes from build-images.sh rather than being built here.
	@cp src/c4bb/images/disk/vfsload.c4r $(C4DOS_IX_DISK)/ 2>/dev/null || true
	@# The CLIMB manifest, not c4ke-root's: this medium carries the
	@# sources as archives, so naming loose .c files would print two
	@# dozen "cannot open" lines on the way up. src/c4bb/fs says why.
	@cp $(SRCS)/c4bb/fs/c4ke-climb.vfs.txt $(C4DOS_IX_DISK)/c4ke.vfs.txt
	@cd $(C4DOS_IX_DISK) && ls -p | grep -v '/$$' > c4dos.dir
	@echo "c4dos-c4ix32: ready -- boot it on c4bb, then LADDER, then IX"
	@echo "                 (or LADDER, dosload c4ke.c4r, and build C4IX from inside it)"

run-c4dos-c4ix32: c4dos32.c4r $(C4DOS_IX_DISK)
	node src/c4bb/sim/cli.js -s -m 640 -i -d $(C4DOS_IX_DISK) c4dos32.c4r

# The breadboard building its own kernel, interactively.
run-c4dos-build32: c4dos32.c4r $(C4DOS_BUILD_DISK32)
	node src/c4bb/sim/cli.js -s -m 160 -i -d $(C4DOS_BUILD_DISK32) c4dos32.c4r

# The whole ladder in one command, and the answer to "how long does a
# machine take to build its own operating system": C4DOS boots on c4bb,
# unpacks the kernel sources onto a RAM disk, preprocesses and compiles
# them, boots the image it just made and shuts it down cleanly. Under
# fifteen seconds. c4fc does the compile alone in about seventy minutes
# -- docs/compiler-on-the-board.md has the instruction counts for why.
test-c4dos-build32: c4dos32.c4r $(C4DOS_BUILD_DISK32)
	printf 'BUILD\nRUN dosload.c4r c4ke.c4r\n\\q\n' \
	  | node src/c4bb/sim/cli.js -s -m 192 -d $(C4DOS_BUILD_DISK32) c4dos32.c4r \
	  > .c4dos_b32.log 2>&1
	grep -q "Kernel ready" .c4dos_b32.log
	grep -q "C4SH - The C4 SHell" .c4dos_b32.log
	grep -q "clean shutdown" .c4dos_b32.log
	@grep -o "c4bb: [0-9]* cycles in [0-9.]*s" .c4dos_b32.log
	@rm -f .c4dos_b32.log
	@echo "test-c4dos-build32: OK"

# LADDER.BAT: the machine rebuilding its own toolchain before it builds
# anything with it. The two checks that matter are the fixed point (the
# compiler c4cc2 builds of itself is the same size as c4cc2) and the
# kernel byte count matching the one the SHIPPED tools produce -- if a
# self-built compiler drifted, the kernel would be the first place it
# showed. Then it boots what it built and shuts down cleanly.
test-c4dos-ladder32: c4dos32.c4r $(C4DOS_BUILD_DISK32)
	printf 'LADDER\nRUN dosload.c4r c4ke.c4r\n\\q\n' \
	  | node src/c4bb/sim/cli.js -s -m 224 -d $(C4DOS_BUILD_DISK32) c4dos32.c4r \
	  > .c4dos_l32.log 2>&1
	@# Nothing is hardcoded: a byte count here would only pin whatever
	@# c4cc happened to compile to on the day. What must hold is that the
	@# compiler is a FIXED POINT (the one it builds of itself is the same
	@# as the one that built it) and that the kernel it produces is the
	@# same as the one the SHIPPED compiler produces from the same .i.
	@sz () { sed -n "s/.*wrote \\([0-9]*\\) bytes to ram:$$1$$/\\1/p" .c4dos_l32.log | head -1; }; \
	 a=$$(sz c4cc2.c4r); b=$$(sz c4cc3.c4r); k=$$(sz c4ke.c4r); z=$$(sz c4ke0.c4r); \
	 [ -n "$$a" ] && [ "$$a" = "$$b" ] || { echo "ladder: not a fixed point ($$a vs $$b)"; exit 1; }; \
	 [ -n "$$k" ] && [ "$$k" = "$$z" ] || { echo "ladder: self-built compiler drifted ($$k vs $$z)"; exit 1; }; \
	 echo "ladder: c4cc is a fixed point at $$a bytes; both kernels are $$k bytes"
	grep -q "C4SH - The C4 SHell" .c4dos_l32.log
	grep -q "clean shutdown" .c4dos_l32.log
	@grep -o "c4bb: [0-9]* cycles in [0-9.]*s" .c4dos_l32.log
	@rm -f .c4dos_l32.log
	@echo "test-c4dos-ladder32: OK"

# Response files: @NAME on a command line is replaced by the words in
# NAME. The point is that C4DOS holds fifteen tokens and linking C4IX
# needs sixteen (docs/compiler-on-the-board.md), so the check is that
# the two links produce THE SAME IMAGE -- an expanded argument has to be
# indistinguishable from one that was typed.
test-respfile: $(C4CC) $(C4RLINK) $(C4M)
	$(C4CC) -o .rf_a.c4o $(TESTS)/test_link_a.c > /dev/null
	$(C4CC) -o .rf_b.c4o $(TESTS)/test_link_b.c > /dev/null
	$(C4RLINK) .rf_a.c4o .rf_b.c4o -o .rf_plain.c4r > /dev/null
	printf '# an object list, as a build would write it\n.rf_a.c4o\n.rf_b.c4o\n' > .rf_objs.txt
	$(C4RLINK) @.rf_objs.txt -o .rf_resp.c4r > /dev/null
	cmp .rf_plain.c4r .rf_resp.c4r
	$(C4M) load-c4r.c -- .rf_resp.c4r | grep -q "b_add(3, 4) = 7"
	@rm -f .rf_a.c4o .rf_b.c4o .rf_plain.c4r .rf_resp.c4r .rf_objs.txt
	@echo "test-respfile: OK"

# B4KE: the C4IX build in miniature, inside C4KE. Two modules compiled
# separately into the RAM filesystem, an object list the BUILD wrote, a
# link that reads that list back as a response file, and the image run.
# Every verb of the format is exercised, and the second run proves the
# skip rule -- the targets are gone with the kernel, so it re-runs; what
# is pinned is that a dry run touches nothing.
test-b4ke: $(BIN_D)/b4ke.c4r $(C4KE_C4R) $(C4M) $(C4R_C4CC) $(C4R_C4RLINK) $(TESTS)/test_link_a.c
	cp $(BIN_D)/b4ke.c4r .
	$(C4M) $(RUN_C4KE) b4ke -f $(SRCS)/c4ke/bin/b4ke-selftest.b4k > .b4ke.log 2>&1
	grep -q "c4rlink: wrote 1898 bytes to ramfs:b4ked.c4r" .b4ke.log
	grep -q "b_add(3, 4) = 7" .b4ke.log
	grep -q "b4ke: 4 ran, 0 skipped, 0 failed" .b4ke.log
	$(C4M) $(RUN_C4KE) b4ke -f $(SRCS)/c4ke/bin/b4ke-selftest.b4k -n > .b4ke_n.log 2>&1
	grep -q "would run: c4rlink.c4r @b4ke.objs -o b4ked.c4r" .b4ke_n.log
	grep -q "b4ke: 0 ran, 0 skipped, 0 failed" .b4ke_n.log
	@rm -f .b4ke.log .b4ke_n.log
	@echo "test-b4ke: OK"

# c4sc: the c4sp Lisp, compiled to C (docs/c4sc-design.md). M1 emits C
# for one file and checks c4lc can compile it. Nothing RUNS yet -- that
# is M2, where the images c4opt rewrites have to come out byte-identical
# to the interpreted pass's. What this pins is the expansion factor,
# which is the risk that decides whether the rest is feasible at all.
test-c4sc: c4sp $(C4LC_LISP) $(C4SC_GEN) src/c4sc/scrt.h
	@# Each unit stands alone: no preprocessing, and every runtime name
	@# is a declaration that c4lc -c turns into a symbol for c4rlink.
	@for u in opt lex pp parse c4r tree gen; do \
	   ./c4sp -R -c 16000000 $(SRCS)/c4sp/lisp/c4lc.lisp -O -c \
	       src/c4sc/c4$${u}_gen.c .c4sc_$$u.c4o > /dev/null || exit 1; \
	   test -s .c4sc_$$u.c4o || exit 1; \
	 done
	@echo "test-c4sc: OK -- four units, all compiled by c4lc -O -c:"
	@for u in opt:c4opt lex:c4lc-lex pp:c4lc-pp parse:c4lc-parse \
	          c4r:c4r tree:c4lc-tree gen:c4lc-gen; do \
	   n=$${u%%:*}; f=$${u##*:}; \
	   echo "  $$f.lisp $$(wc -l < $(SRCS)/c4sp/lisp/$$f.lisp) -> $$(wc -l < src/c4sc/c4$${n}_gen.c) lines"; \
	 done
	@rm -f .c4sc_*.c4o

# M2: the compiled c4opt has to produce the SAME IMAGES as the
# interpreted one. c4sc-host is c4opt-run.lisp with exactly one
# substitution -- c4r.lisp's decoder and encoder still run in the
# interpreter, reached through the bridge in src/c4sc/host.h, and only
# the optimizer is compiled. Built at c4sp's own -O2, so the collector
# is doing the same job here as it does there.
C4SC_GEN := src/c4sc/c4opt_gen.c src/c4sc/c4lex_gen.c \
            src/c4sc/c4pp_gen.c src/c4sc/c4parse_gen.c \
            src/c4sc/c4c4r_gen.c src/c4sc/c4tree_gen.c src/c4sc/c4gen_gen.c
src/c4sc/c4opt_gen.c: c4sp src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4opt.lisp
	./c4sp -R -c 8000000 src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4opt.lisp $@ opt > /dev/null
src/c4sc/c4lex_gen.c: c4sp src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-lex.lisp
	./c4sp -R -c 8000000 src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-lex.lisp $@ lex > /dev/null
src/c4sc/c4pp_gen.c: c4sp src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-pp.lisp
	./c4sp -R -c 8000000 src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-pp.lisp $@ pp > /dev/null
src/c4sc/c4parse_gen.c: c4sp src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-parse.lisp
	./c4sp -R -c 8000000 src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-parse.lisp $@ parse > /dev/null
src/c4sc/c4c4r_gen.c: c4sp src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4r.lisp
	./c4sp -R -c 8000000 src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4r.lisp $@ c4r > /dev/null
src/c4sc/c4tree_gen.c: c4sp src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-tree.lisp
	./c4sp -R -c 8000000 src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-tree.lisp $@ tree > /dev/null
src/c4sc/c4gen_gen.c: c4sp src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-gen.lisp
	./c4sp -R -c 8000000 src/c4sc/c4sc.lisp $(SRCS)/c4sp/lisp/c4lc-gen.lisp $@ gen > /dev/null

c4sc-host: c4sp $(C4SC_GEN) src/c4sc/c4sc-host.c src/c4sc/host.h src/c4sc/scrt.h
	gcc $(EXTRA_CC) -O2 -fwrapv -fno-omit-frame-pointer -g -Iinclude -I. -o c4sc-host src/c4sc/c4sc-host.c

# The corpus is test-c4sp-opt's, plus the two biggest images in the tree
# -- c4cc.c4r is 423 KB and 26,818 instructions, which is where a
# transliteration bug that survives factorial shows up.
C4SC_CORPUS := factorial.c4r $(C4R_C4CC) $(C4KE_C4R) $(TESTS)/tests.c4r \
               $(TESTS)/test_globals.c4r $(TESTS)/mandel.c4r
test-c4sc-run: c4sc-host c4sp $(C4SC_CORPUS)
	@fail=0; for img in $(C4SC_CORPUS); do \
	   b=$$(basename $$img); \
	   ./c4sp -R -c 16000000 $(SRCS)/c4sp/lisp/c4opt-run.lisp $$img .c4sc_i.c4r > .c4sc_i.log 2>&1; \
	   ./c4sc-host $$img .c4sc_c.c4r > .c4sc_c.log 2>&1; \
	   cmp -s .c4sc_i.c4r .c4sc_c.c4r || { echo "test-c4sc-run: $$b IMAGE DIFFERS"; fail=1; continue; }; \
	   grep -E '^;; c4opt: [0-9]|^;;   fold' .c4sc_i.log > .c4sc_i.n; \
	   grep -E '^;; c4opt: [0-9]|^;;   fold' .c4sc_c.log > .c4sc_c.n; \
	   cmp -s .c4sc_i.n .c4sc_c.n \
	     || { echo "test-c4sc-run: $$b COUNTERS DIFFER"; fail=1; continue; }; \
	   echo "  ok: $$b (image and pass counts identical)"; \
	 done; \
	 [ $$fail = 0 ] || exit 1
	@# and the fuse pass, which rewrites a third of the instructions
	./c4sp -R -c 16000000 $(SRCS)/c4sp/lisp/c4opt-run.lisp -mfuse factorial.c4r .c4sc_i.c4r > /dev/null
	./c4sc-host -mfuse factorial.c4r .c4sc_c.c4r > /dev/null
	cmp .c4sc_i.c4r .c4sc_c.c4r
	@rm -f .c4sc_i.c4r .c4sc_c.c4r .c4sc_i.log .c4sc_c.log .c4sc_i.n .c4sc_c.n
	@echo "test-c4sc-run: OK"

# M3: the lexer. The bar is the token DUMP, not the count -- the count
# was right while every keyword was lexing as an identifier, because a
# quoted table printed back and re-read had turned its strings into
# atoms. The sweep is the same corpus test-c4fc uses for c4fc's lexer.
C4SC_LEX_SWEEP := c4.c c4m.c c4l.c load-c4r.c $(SRCS)/c4th/c4th.c \
                  $(SRCS)/c4cc/asm-c4r.c $(SRCS)/c4dos/c4dos.c \
                  $(SRCS)/c4ix/vfs.c $(SRCS)/c4ix/sched.c \
                  $(SRCS)/c4or1k/cpu.c $(TESTS)/mandel.c $(TESTS)/tests.c \
                  $(SRCS)/c4cc/c4cc.c
test-c4sc-lex: c4sc-host c4sp
	./c4sc-host tokens $(TESTS)/c4lc_lex_sample.c > .c4sc_lex.txt
	head -n -1 $(SRCS)/c4sp/tests/expected/c4lc-tokens.txt | cmp - .c4sc_lex.txt
	./c4sc-host tokens -conforming $(TESTS)/c4lc_lex_sample.c > .c4sc_lex.txt
	head -n -1 $(SRCS)/c4sp/tests/expected/c4lc-tokens-conforming.txt | cmp - .c4sc_lex.txt
	./c4sc-host tokens -count $(SRCS)/c4cc/c4cc.c | grep -q "^tokens 15125$$"
	@fail=0; for f in $(C4SC_LEX_SWEEP); do \
	   ./c4sp -R -c 33554432 $(SRCS)/c4sp/lisp/c4lc-tokens.lisp $$f > .c4sc_i.txt 2>&1; \
	   head -n -1 .c4sc_i.txt > .c4sc_i2.txt; \
	   ./c4sc-host -c 33554432 tokens $$f > .c4sc_c.txt 2>&1; \
	   cmp -s .c4sc_i2.txt .c4sc_c.txt \
	     || { echo "test-c4sc-lex: $$f DIFFERS"; fail=1; continue; }; \
	   echo "  ok: $$f ($$(wc -l < .c4sc_c.txt) tokens)"; \
	 done; [ $$fail = 0 ] || exit 1
	@rm -f .c4sc_lex.txt .c4sc_i.txt .c4sc_i2.txt .c4sc_c.txt
	@echo "test-c4sc-lex: OK"

# M4: the rest of the front end. The preprocessor's bar is the one
# test-c4fc uses -- gcc preprocesses and we lex, we preprocess and we
# lex, and the two token streams must agree -- run over all twelve C4IX
# modules. The parser's is the committed AST dump.
test-c4sc-front: c4sc-host c4sp
	@fail=0; for m in $(C4IX_MODS); do \
	   f=$(C4IX_SRC)/$$m.c; \
	   gcc -E -Iinclude -I. -I$(C4IX_SRC) -DC4CC=1 -D__c4__=1 -D__C4CC__=1 \
	       -D__c4cc__=1 -C $$f > .c4sc_g.i 2>/dev/null || exit 1; \
	   ./c4sc-host -c 8000000 pptok .c4sc_g.i > .c4sc_a.txt 2>&1; \
	   ./c4sc-host -c 8000000 pp -I include -I . -I $(C4IX_SRC) \
	       -D C4CC=1 -D __c4__=1 -D __C4CC__=1 -D __c4cc__=1 -D __GNUC__=1 \
	       $$f > .c4sc_b.txt 2>&1; \
	   cmp -s .c4sc_a.txt .c4sc_b.txt \
	     || { echo "test-c4sc-front: pp differs from gcc -E on $$m"; fail=1; continue; }; \
	   echo "  pp ok: $$m ($$(wc -l < .c4sc_a.txt) tokens)"; \
	 done; [ $$fail = 0 ] || exit 1
	@# the parser: the committed dump, and then that every C4IX module
	@# parses to the same declaration count as the interpreter's
	./c4sc-host -c 8000000 ast $(TESTS)/c4lc_lex_sample.c > .c4sc_ast.txt
	head -n -1 $(SRCS)/c4sp/tests/expected/c4lc-ast.txt | cmp - .c4sc_ast.txt
	@fail=0; for m in $(C4IX_MODS); do \
	   gcc -E -Iinclude -I. -I$(C4IX_SRC) -DC4CC=1 -D__c4__=1 -D__C4CC__=1 \
	       -D__c4cc__=1 -C $(C4IX_SRC)/$$m.c > .c4sc_g.i 2>/dev/null; \
	   a=`./c4sc-host -c 16000000 ast -check .c4sc_g.i 2>&1 | tail -1`; \
	   b=`./c4sp -R -c 16000000 $(SRCS)/c4sp/lisp/c4lc-ast.lisp -check .c4sc_g.i 2>&1 | head -1`; \
	   [ "$$a" = "$$b" ] || { echo "test-c4sc-front: parse differs on $$m"; fail=1; continue; }; \
	   echo "  parse ok: $$m ($$a)"; \
	 done; [ $$fail = 0 ] || exit 1
	@rm -f .c4sc_g.i .c4sc_a.txt .c4sc_b.txt .c4sc_ast.txt
	@echo "test-c4sc-front: OK"

# M5: the back end, and the bar is the OUTPUT. Every image the compiled
# compiler writes must be byte-identical to the interpreted one's, over
# the same corpus test-c4lc uses, plain and -O; then the twelve C4IX
# modules as objects; then those objects linked into a kernel that must
# equal the committed c4ix.c4r and boot. Nothing of the pipeline is
# interpreted any more -- lexer, preprocessor, parser, tree passes, code
# generator, peephole passes and the .c4r writer are all compiled, and
# only file:read and file:write are still c4sp's.
C4SC_BACK_DIFF := c4_jailbreak factorial hello multifun puts reverse \
                  test_basic test_continue test_coop_switch test_globals \
                  test_malloc test-order test-ptrs test_static test_switch \
                  tests c4lc_l2 global test_gcscan test-oisc test_printf
test-c4sc-back: c4sc-host c4sp $(C4RLINK) $(C4M)
	@fail=0; for b in $(C4SC_BACK_DIFF); do \
	   f=$(TESTS)/$$b.c; \
	   for fl in "" "-O"; do \
	     ./c4sp -R -c 16000000 $(SRCS)/c4sp/lisp/c4lc.lisp $$fl $$f .c4sc_i.c4r >/dev/null 2>&1; \
	     ./c4sc-host -c 16000000 compile $$fl $$f .c4sc_c.c4r >/dev/null 2>&1; \
	     cmp -s .c4sc_i.c4r .c4sc_c.c4r \
	       || { echo "test-c4sc-back: $$b $$fl DIFFERS"; fail=1; }; \
	   done; \
	 done; [ $$fail = 0 ] || exit 1
	@echo "  corpus ok: $(words $(C4SC_BACK_DIFF)) programs, plain and -O, byte-identical"
	@fail=0; for m in $(C4IX_MODS); do \
	   ./c4sp -R -c 33554432 $(SRCS)/c4sp/lisp/c4lc.lisp -O -c -I $(C4IX_SRC) \
	       $(C4IX_SRC)/$$m.c .c4sc_i.c4o >/dev/null 2>&1; \
	   ./c4sc-host -c 33554432 compile -O -c -I $(C4IX_SRC) \
	       $(C4IX_SRC)/$$m.c .c4sc_$$m.c4o >/dev/null 2>&1; \
	   cmp -s .c4sc_i.c4o .c4sc_$$m.c4o \
	     || { echo "test-c4sc-back: $$m.c4o DIFFERS"; fail=1; continue; }; \
	   echo "  obj ok: $$m ($$(wc -c < .c4sc_$$m.c4o) bytes)"; \
	 done; [ $$fail = 0 ] || exit 1
	$(C4RLINK) $(patsubst %,.c4sc_%.c4o,$(C4IX_MODS)) -o .c4sc_ix.c4r
	$(MAKE) c4ix.c4r
	cmp .c4sc_ix.c4r c4ix.c4r
	$(C4M) load-c4r.c -- .c4sc_ix.c4r --demo 2>&1 | grep -q "shutdown complete"
	@rm -f .c4sc_i.c4r .c4sc_c.c4r .c4sc_i.c4o .c4sc_*.c4o .c4sc_ix.c4r
	@echo "test-c4sc-back: OK -- the kernel it built is the committed one, and it boots"

# M6: the compiler as an IMAGE. The seven generated units and the driver
# are compiled to objects and linked by c4rlink, so c4lc -- lexer,
# preprocessor, parser, tree passes, code generator, peephole passes and
# the .c4r writer -- becomes one .c4r that runs anywhere c4m does. This
# is what M8 puts on the board.
#
# The driver goes through gcc -E first for the reason c4sp.c4r does:
# c4lc's own preprocessor refuses a function-like macro named without an
# argument list, and c4.h has one.
C4SC_UNITS := opt lex pp parse c4r tree gen
c4sc.c4r: c4sp $(C4LC_LISP) $(C4RLINK) $(C4SC_GEN) src/c4sc/c4sc-main.c \
          src/c4sc/body.h src/c4sc/host.h src/c4sc/scrt.h
	$(PREPROC) -DC4SP_DOS=1 src/c4sc/c4sc-main.c > .c4sc_main.c
	$(C4SPLC) $(SRCS)/c4sp/lisp/c4lc.lisp -O -c .c4sc_main.c .c4sc_o_main.c4o > /dev/null
	@for u in $(C4SC_UNITS); do \
	   echo "  unit: $$u"; \
	   $(C4SPLC) -c 33554432 $(SRCS)/c4sp/lisp/c4lc.lisp -O -c \
	       src/c4sc/c4$${u}_gen.c .c4sc_o_$$u.c4o > /dev/null || exit 1; \
	 done
	$(C4RLINK) .c4sc_o_main.c4o $(patsubst %,.c4sc_o_%.c4o,$(C4SC_UNITS)) -o $@
	@rm -f .c4sc_main.c .c4sc_o_*.c4o
	@echo "c4sc.c4r: $$(wc -c < $@) bytes"

test-c4sc-image: c4sc.c4r c4sc-host $(C4M) c4sp
	@# the image runs, and computes what the interpreter computes
	$(C4M) load-c4r.c -- c4sc.c4r -c 4000000 tokens -count $(SRCS)/c4cc/c4cc.c \
	  | grep -q "^tokens 15125$$"
	@echo "  hosted lex: tokens 15125"
	@# a real compile, hosted: the image it produces has to be the size
	@# the native build produces. Bare c4m has no write syscall -- the
	@# same limit c4lc has -- so the size line is printed before the
	@# write is attempted and that is what is compared.
	@fail=0; for m in boot va host sl4b; do \
	   n=`./c4sc-host -c 8000000 compile -O -c -I $(C4IX_SRC) $(C4IX_SRC)/$$m.c .c4sc_n.c4o 2>&1 \
	      | sed -n 's/.* - \([0-9]*\) bytes.*/\1/p'`; \
	   h=`$(C4M) load-c4r.c -- c4sc.c4r -c 8000000 compile -O -c -I $(C4IX_SRC) \
	      $(C4IX_SRC)/$$m.c out.c4o 2>&1 | sed -n 's/.* - \([0-9]*\) bytes.*/\1/p'`; \
	   [ -n "$$n" ] && [ "$$n" = "$$h" ] \
	     || { echo "test-c4sc-image: $$m hosted $$h vs native $$n"; fail=1; continue; }; \
	   echo "  hosted compile: $$m ($$h bytes, same as native)"; \
	 done; [ $$fail = 0 ] || exit 1
	@rm -f .c4sc_n.c4o
	@echo "test-c4sc-image: OK"

# M7: the fixed point. gen1 is c4sc.lisp compiled by the INTERPRETED
# c4sc; gen2 is c4sc.lisp compiled by gen1; gen3 by gen2. All three must
# be the same bytes. The first equality is the interesting one: the
# compiler running on c4sp's evaluator and the same compiler running as
# compiled C emit the same text.
#
# c4sc-self is its own binary because c4sc.lisp defines `second` and
# `third` and so does c4r.lisp -- two units that both define a name
# cannot share a link, and the mangling is name-based on purpose so that
# a call from one unit to another resolves at all.
c4sc-self: c4sp src/c4sc/c4sc.lisp src/c4sc/c4sc-self.c src/c4sc/scrt.h
	./c4sp -R -c 8000000 src/c4sc/c4sc.lisp src/c4sc/c4sc.lisp src/c4sc/c4self_gen.c c4sc > /dev/null
	gcc $(EXTRA_CC) -O2 -fwrapv -fno-omit-frame-pointer -g -Iinclude -I. -o c4sc-self src/c4sc/c4sc-self.c

test-c4sc-self: c4sc-self $(C4SC_GEN)
	cp src/c4sc/c4self_gen.c .c4sc_gen1.c
	./c4sc-self src/c4sc/c4sc.lisp .c4sc_gen2.c c4sc > /dev/null
	cmp .c4sc_gen1.c .c4sc_gen2.c
	@echo "  gen1 == gen2 ($$(wc -c < .c4sc_gen2.c) bytes)"
	cp .c4sc_gen2.c src/c4sc/c4self_gen.c
	gcc $(EXTRA_CC) -O2 -fwrapv -fno-omit-frame-pointer -g -Iinclude -I. -o .c4sc_self2 src/c4sc/c4sc-self.c
	./.c4sc_self2 src/c4sc/c4sc.lisp .c4sc_gen3.c c4sc > /dev/null
	cmp .c4sc_gen2.c .c4sc_gen3.c
	@echo "  gen2 == gen3 -- fixed point"
	@# and the compiled compiler has to compile the seven units exactly
	@# as the interpreted one does
	@fail=0; for spec in opt:c4opt lex:c4lc-lex pp:c4lc-pp parse:c4lc-parse \
	                     c4r:c4r tree:c4lc-tree gen:c4lc-gen; do \
	   u=$${spec%%:*}; f=$${spec##*:}; \
	   ./.c4sc_self2 $(SRCS)/c4sp/lisp/$$f.lisp .c4sc_s.c $$u > /dev/null; \
	   cmp -s .c4sc_s.c src/c4sc/c4$${u}_gen.c \
	     || { echo "test-c4sc-self: $$u differs from the interpreted c4sc"; fail=1; }; \
	 done; [ $$fail = 0 ] || exit 1
	@echo "  and it emits the same seven units the interpreter does"
	@rm -f .c4sc_gen1.c .c4sc_gen2.c .c4sc_gen3.c .c4sc_self2 .c4sc_s.c
	@echo "test-c4sc-self: OK"

# M8: the compiler on the breadboard. Eight objects at 32 bits, linked,
# then run through c4opt's fuse pass -- which is why the FUSED image is
# made from the linked one rather than from fused objects: fusion
# rewrites instruction sequences and c4opt does that to a whole image,
# the same path build-images.sh already uses for factorial-fused.
#
# The result runs on c4bb, c4mp and oisc4 but NOT on c4m, which has only
# three of the ten fused opcodes -- so the unfused image is kept as
# well, and that is the one the byte-identity check uses.
c4sc32.c4r: c4sp32 c4rlink32 $(C4LC_LISP) $(C4SC_GEN) src/c4sc/c4sc-main.c \
            src/c4sc/body.h src/c4sc/host.h src/c4sc/scrt.h
	$(PREPROC) -DC4SP_DOS=1 src/c4sc/c4sc-main.c > .c4sc32_main.c
	./c4sp32 -R -c 33554432 $(SRCS)/c4sp/lisp/c4lc.lisp -O -c .c4sc32_main.c .c4sc32_o_main.c4o > /dev/null
	@for u in $(C4SC_UNITS); do \
	   echo "  unit: $$u"; \
	   ./c4sp32 -R -c 33554432 $(SRCS)/c4sp/lisp/c4lc.lisp -O -c \
	       src/c4sc/c4$${u}_gen.c .c4sc32_o_$$u.c4o > /dev/null || exit 1; \
	 done
	./c4rlink32 .c4sc32_o_main.c4o $(patsubst %,.c4sc32_o_%.c4o,$(C4SC_UNITS)) -o c4sc32.c4r
	@rm -f .c4sc32_main.c .c4sc32_o_*.c4o
	@echo "c4sc32.c4r: $$(wc -c < c4sc32.c4r) bytes"

c4sc32-fused.c4r: c4sc32.c4r c4sp32 $(C4LC_LISP)
	./c4sp32 -R -c 33554432 $(SRCS)/c4sp/lisp/c4opt-run.lisp -mfuse c4sc32.c4r $@ > /dev/null
	@echo "c4sc32-fused.c4r: $$(wc -c < $@) bytes"

# One module on the board, and the object it reports has to be the one
# c4lc reports at 32 bits. boot.c because it is the smallest -- the
# whole twelve is four and a half minutes and belongs in the tracker,
# not in a test.
test-c4sc-board: c4sc32-fused.c4r c4sc32.c4r c4sp32 c4m32
	@rm -rf .c4sc_bd && mkdir -p .c4sc_bd/src/c4ix
	@cp $(C4IX_SRC)/*.c $(C4IX_SRC)/*.h .c4sc_bd/src/c4ix/ && cp -r include .c4sc_bd/
	./c4sp32 -R -c 16000000 $(SRCS)/c4sp/lisp/c4lc.lisp -O -c -I $(C4IX_SRC) \
	    $(C4IX_SRC)/boot.c .c4sc_bd_i.c4o > /dev/null
	@n=`wc -c < .c4sc_bd_i.c4o`; \
	 h=`node src/c4bb/sim/cli.js -s -m 512 -d .c4sc_bd c4sc32-fused.c4r -c 8000000 \
	      compile -O -c -I src/c4ix src/c4ix/boot.c out.c4o 2>&1 \
	    | sed -n 's/.* - \([0-9]*\) bytes.*/\1/p'`; \
	 [ -n "$$h" ] && [ "$$n" = "$$h" ] \
	   || { echo "test-c4sc-board: board $$h vs c4lc $$n"; exit 1; }; \
	 echo "  board compile: boot.c $$h bytes, same as c4lc at 32 bits"
	@rm -rf .c4sc_bd .c4sc_bd_i.c4o
	@echo "test-c4sc-board: OK"

# The whole climb, non-interactive: C4DOS boots, IX compiles the twelve
# C4IX modules with the compiled c4lc and links them, dosload boots the
# kernel that came out, and C4IX reaches its own shell. About four
# minutes of simulated hardware, so it is not in any default chain.
# LADDER's half is pinned separately by test-c4dos-ladder32.
test-c4dos-c4ix32: c4dos32.c4r $(C4DOS_IX_DISK)
	printf 'IX\nRUN dosload.c4r c4ix.c4r\nls\nexit\n' \
	  | node src/c4bb/sim/cli.js -s -m 128 -d $(C4DOS_IX_DISK) c4dos32.c4r \
	  > .c4dos_ix.log 2>&1
	grep -q "c4rlink: wrote .* to ram:c4ix.c4r" .c4dos_ix.log
	grep -q "C4IX booting" .c4dos_ix.log
	grep -q "c4ix-sh -- 'help' for builtins" .c4dos_ix.log
	grep -q "shutdown complete" .c4dos_ix.log
	@grep -o "c4bb: [0-9]* cycles in [0-9.]*s" .c4dos_ix.log
	@rm -f .c4dos_ix.log
	@echo "test-c4dos-c4ix32: OK -- C4DOS built C4IX and booted it"

# c4cc's for statement. It had never worked -- see src/tests/test_for.c
# -- so this pins it against gcc's output for the same program, which is
# how every other language feature here is checked.
test-c4cc-for: $(C4CC) $(C4M) $(TESTS)/test_for.c
	$(C4CC) -o .c4cc_for.c4r $(TESTS)/test_for.c > /dev/null
	$(C4M) load-c4r.c -- .c4cc_for.c4r | cmp - $(TESTS)/expected/test_for.txt
	@rm -f .c4cc_for.c4r
	@echo "test-c4cc-for: OK"

# An interactive C4DOS session, which is what the thing is for: an A># An interactive C4DOS session, which is what the thing is for: an A>
# prompt, DIR/TYPE/RUN/TIME/MEM/VER, EXIT to halt. `cd` because the
# working directory IS the disk.
run-c4dos: $(C4M) c4dos-clock.c4r $(C4DOS_DISK)
	@cd $(C4DOS_DISK) && $(CURDIR)/c4m $(CURDIR)/load-c4r.c -- $(CURDIR)/c4dos-clock.c4r
# The same session on the ORIGINAL interpreter, through c4l: unmodified
# c4 runs the loader runs DOS runs your program. Clockless, so TIME
# says so -- an image with TIME in it is one c4l would refuse.
run-c4dos-c4: $(C4) c4dos.c4r $(C4DOS_DISK)
	@cd $(C4DOS_DISK) && $(CURDIR)/c4 $(CURDIR)/c4l.c $(CURDIR)/c4dos.c4r
# And on the breadboard machine, which is where an embedder meets it.
run-c4dos-bb: c4dos32.c4r $(C4DOS_DISK32)
	node src/c4bb/sim/cli.js -i -d $(C4DOS_DISK32) c4dos32.c4r

# The board in a browser. Served from the REPO ROOT, not src/c4bb/web:
# web/app.js fetches drive and image paths that are repo-root-relative,
# so a server rooted at the page's own directory comes up with an empty
# drives panel and no obvious reason why. There is nothing to install --
# no node_modules anywhere in this repo -- so this is python's http
# server with a name attached to it.
C4BB_PORT ?= 8471
serve-c4bb:
	@echo "c4bb: http://localhost:$(C4BB_PORT)/src/c4bb/web/index.html"
	@echo "      pick c4ix32 or c4ke32, press Turbo, click the terminal, type."
	@python3 -m http.server $(C4BB_PORT)

# The whole HOMEWARD ladder, end to end: C4DOS builds C4KE, dosload
# hands the machine over, the kernel boots the init it was just handed,
# and both toolchains complete a compile-link round trip in the RAM
# filesystem. MINUTES long, and deliberately not part of any other
# suite -- docs/homeward-ladder.md is the tracker.
test-ladder: cpp $(C4) $(C4M) $(C4CC) c4sp
	@bash src/c4bb/tests/test-ladder.sh

# C4DOS, the single-tasking trap-free DOS (docs/c4dos-design.md).
# Strict-c4 core: the same image runs on c4bb, under native c4m, and
# under PLAIN c4 via c4l.c (the clockless build) - test-c4dos checks
# all three plus the c4-inside-DOS-inside-c4bb nesting.
test-c4dos: cpp $(C4) $(C4M) $(C4CC) $(TESTS)/hello.c4r
	bash src/c4dos/tests/test-c4dos.sh

# c4sp, the Lisp interpreter (docs/c4sp-design.md)
C4SP_SRCS := src/c4sp/c4sp.c src/c4sp/include/cell.h src/c4sp/include/gc.h \
             src/c4sp/include/cells.h src/c4sp/include/atoms.h \
             src/c4sp/include/read.h src/c4sp/include/stdlib.h \
             src/c4sp/include/eval.h src/c4sp/include/cek.h \
             include/c4_float.h
# -O2 is safe since gc_collect() spills the callee-saved registers with
# setjmp and scans the buffer along with the stack (src/c4sp/include/gc.h).
# Before that it was NOT: an optimizing gcc keeps the only reference to a
# live cell in a register, the conservative scan misses it, and the cell is
# collected -- an -O2 build without the spill dies on gcloop.lisp with
# "undefined variable: n" and crashes every real compile. Under the C4 VM
# there are no such registers and the scan is exact, so none of this
# applies to c4sp.c4r.
C4TH_SRCS := src/c4th/c4th.c src/c4th/include/mem.h src/c4th/include/dict.h \
             src/c4th/include/io.h src/c4th/include/inner.h \
             src/c4th/include/prim.h src/c4th/include/num.h \
             src/c4th/include/outer.h src/c4th/forth/core.f \
             src/c4th/forth/asm.f src/c4th/forth/peep.f \
             src/c4th/forth/native.f
# c4th (docs/c4th-design.md): a Forth for C4, built two ways from one
# source exactly as c4sp is. -O2 needs no apology here -- c4th has no
# collector scanning the stack, so nothing pins the native build open.
c4th: $(C4TH_SRCS)
	gcc $(EXTRA_CC) -O2 -fwrapv -fno-omit-frame-pointer -g -idirafter include -I. -o c4th src/c4th/c4th.c
c4th.c4r: $(C4CC) $(C4TH_SRCS)
	$(PREPROC) src/c4th/c4th.c | $(C4CC) -o c4th.c4r - > /dev/null

# B1 pins the engine itself, before there is any outer interpreter to read
# Forth with: the driver hand-threads a body cell by cell for 10! and runs
# it. The check is not just the number -- a body that left junk on either
# stack would still print 3628800 -- so the selftest also requires both
# stacks to come back empty.
#
# Three hosts, one golden. The native build is gcc's; the .c4r build runs
# under c4m; and the same image runs inside C4KE, which is the one that
# proves the indirect call through a local really is a JSRS the kernel can
# host, rather than something only gcc's cast macro makes work.
test-c4th: c4th c4th.c4r $(C4M) c4mp $(OISC4) c4sp $(C4KE_C4R)
	./c4th -selftest | cmp - src/c4th/tests/expected/b1.txt
	$(C4M) load-c4r.c -- c4th.c4r -selftest | cmp - src/c4th/tests/expected/b1.txt
	$(C4M) load-c4r.c -- $(C4KE_C4R) c4th.c4r -selftest | grep -q "selftest ok"
	# B2: the outer interpreter. b2.f walks every primitive group -- there
	# are no control structures yet, since IF/THEN and friends are Forth
	# written in Forth and arrive with core.f at B3 -- and the golden is
	# shared by the native and .c4r builds, so a divergence between the
	# two hosts is a failing cmp rather than something noticed later.
	# BASE is checked through HEX/DECIMAL rather than by storing 10 into
	# BASE, because 10 typed while hex is sixteen; . prints in BASE.
	./c4th src/c4th/tests/b2.f | cmp - src/c4th/tests/expected/b2.txt
	$(C4M) load-c4r.c -- c4th.c4r src/c4th/tests/b2.f | cmp - src/c4th/tests/expected/b2.txt
	./c4th -e ': SQ DUP * ; 7 SQ . CR' | grep -q "^49"
	$(C4M) load-c4r.c -- c4th.c4r -e ': SQ DUP * ; 7 SQ . CR' | grep -q "^49"
	# B3: the Forth-2012 CORE word set, against the standard suite.
	# src/c4th/tests/{tester.fr,core.fr} are vendored verbatim from Gerry
	# Jackson's Forth-2012 test suite (see the README there) and are NOT
	# to be edited: the value of a standards suite is precisely that we
	# did not write it and it does not know what c4th happens to
	# implement.
	#
	# Two checks, deliberately. The transcript is compared against a
	# golden, so a change in WHICH things pass is a diff rather than
	# silent drift; and independently the count of failure lines must be
	# zero, so the golden can never quietly bless a regression. The suite
	# is self-verifying -- it prints only on failure -- so this needs no
	# reference Forth. A line is piped in because the ACCEPT section
	# genuinely reads the terminal.
	echo c4th | ./c4th src/c4th/forth/core.f src/c4th/tests/tester.fr src/c4th/tests/core.fr > .c4th_core
	cmp .c4th_core src/c4th/tests/expected/core-64.txt
	test 0 = `grep -c "INCORRECT RESULT\|WRONG NUMBER OF RESULTS" .c4th_core`
	grep -q "End of Core word set tests" .c4th_core
	echo c4th | $(C4M) load-c4r.c -- c4th.c4r src/c4th/forth/core.f src/c4th/tests/tester.fr src/c4th/tests/core.fr | cmp - src/c4th/tests/expected/core-64.txt
	echo c4th | $(C4M) load-c4r.c -- $(C4KE_C4R) c4th.c4r src/c4th/forth/core.f src/c4th/tests/tester.fr src/c4th/tests/core.fr | grep -q "End of Core word set tests"
	# B4: the assembler and the peephole.
	#
	# asm.f is checked against c4cc rather than against itself: the same
	# program is compiled by c4cc and hand-assembled in Forth, and the two
	# instruction sequences must match. Addresses are masked -- they
	# depend on where the segments landed, and the claim is about the
	# encoding, not the layout.
	./c4cc -o .c4th_fact.c4r src/c4th/tests/fact.c > /dev/null
	./c4rdump -c .c4th_fact.c4r 2>/dev/null | grep -E '^0x' | sed -E 's/^0x[0-9a-f]+: +//; s/[0-9]{7,}/*/; s/ +$$//' > .c4th_ccasm
	./c4th src/c4th/forth/core.f src/c4th/forth/asm.f src/c4th/tests/fact.f | sed -E 's/ +$$//' | cmp - .c4th_ccasm
	# SEE: the same disassembler pointed at COMPILED Forth rather than at
	# hand-written assembly. The listing is where the accumulator model
	# stops being a paragraph -- DROP is `IMM 0; ADD` because C4's ALU
	# already pops, and a flag is `PSH; IMM -1; MUL` because Forth wants
	# -1 where C4 gives 1.
	./c4th src/c4th/forth/core.f src/c4th/forth/ext.f src/c4th/forth/locals.f \
	       src/c4th/forth/asm.f src/c4th/forth/peep.f \
	       src/c4th/forth/native.f src/c4th/tests/see.f | cmp - src/c4th/tests/expected/see.txt
	# Each peephole rule fires, and behaviour is unchanged -- including
	# the cases where compaction moves a branch target.
	./c4th src/c4th/forth/core.f src/c4th/forth/peep.f src/c4th/tests/b4.f | cmp - src/c4th/tests/expected/b4.txt
	# The real check: run the whole Forth-2012 CORE suite again with the
	# peephole applied to EVERY definition, including the test harness's
	# own. A peephole exercised only by its own tests is one nobody
	# trusts; the transcript must come out byte-identical to the
	# unoptimized golden.
	echo c4th | ./c4th src/c4th/forth/core.f src/c4th/forth/peep.f src/c4th/tests/peepon.f src/c4th/tests/tester.fr src/c4th/tests/core.fr | cmp - src/c4th/tests/expected/core-64.txt
	echo c4th | $(C4M) load-c4r.c -- c4th.c4r src/c4th/forth/core.f src/c4th/forth/peep.f src/c4th/tests/peepon.f src/c4th/tests/tester.fr src/c4th/tests/core.fr | cmp - src/c4th/tests/expected/core-64.txt
	# B5: the native backend. Only under c4m -- INVOKE is an indirect
	# call through generated code, and the gcc build has no VM to invoke
	# into, so natively there is nothing to invoke.
	#
	# The threaded engine is the oracle: it is the one that passes the
	# Forth-2012 CORE suite. Every word is compiled, CALLED, and its
	# answer compared against the threaded one -- so this checks the
	# emitted code runs, not merely that the compiler did not complain.
	#
	# Two checks, as for the CORE suite. The transcript is pinned, and
	# independently every line must end in "ok" except the four that are
	# meant to decline: an IF whose arms leave different depths, a
	# recursive word, 2! (which has no single result to compare), and a
	# word calling a primitive the backend cannot emit. A backend that
	# quietly emitted worse or wrong code would be worse than none, so
	# the declines are part of the specification.
	$(C4M) load-c4r.c -- c4th.c4r src/c4th/forth/core.f src/c4th/forth/asm.f src/c4th/forth/native.f src/c4th/tests/b5.f > .c4th_b5
	cmp .c4th_b5 src/c4th/tests/expected/b5.txt
	test 4 = `grep -vc " ok$$" .c4th_b5`
	test 0 = `grep -c "MISMATCH" .c4th_b5`
	# The same suite again with the fused opcodes on -- under plain
	# c4m, because the three the backend uses (LDL, STL, POPA) are the
	# three c4m has. That is the split, checked from this side; the
	# other side is test-fuse, where c4m must REFUSE an image using the
	# seven it does not have. Byte-identical transcript either way:
	# same answers, fewer instructions.
	$(C4M) load-c4r.c -- c4th.c4r src/c4th/forth/core.f src/c4th/forth/asm.f src/c4th/forth/native.f src/c4th/tests/nopc.f src/c4th/tests/b5.f | cmp - src/c4th/tests/expected/b5.txt
	# Every fused opcode against the sequence it replaces, at the VM
	# level: each is hand-assembled into a tiny function, its unfused
	# twin into another, both are called and the answers must agree.
	# c4th's assembler is the only thing in the tree that can lay down
	# an arbitrary instruction and then call it. Under c4mp, because
	# this one exercises all ten. docs/fused-opcodes.md.
	./c4mp c4th.c4r src/c4th/forth/core.f src/c4th/forth/asm.f src/c4th/tests/fused.f > .c4th_fused
	cmp .c4th_fused src/c4th/tests/expected/fused.txt
	test 0 = `grep -c MISMATCH .c4th_fused`
	# B5d: the metacompiler. c4th compiles a program to a standalone
	# .c4r and the image must behave exactly as c4th's own threaded
	# engine did running the same source -- on every machine that can
	# run it. The interpreter is the oracle here for the reason it was
	# at B5: it is the engine that passes the Forth-2012 CORE suite.
	#
	# Native c4th only: writing the file needs a write syscall and the
	# C4 VM has none.
	./c4th src/c4th/forth/core.f src/c4th/forth/asm.f src/c4th/forth/native.f src/c4th/forth/c4r.f src/c4th/forth/trt.f src/c4th/tests/b5d.f > .c4th_b5di
	cmp .c4th_b5di src/c4th/tests/expected/b5d.txt
	$(C4M) load-c4r.c -- .c4th_b5d.c4r | cmp - .c4th_b5di
	./c4mp .c4th_b5d.c4r              | cmp - .c4th_b5di
	$(OISC4) .c4th_b5d.c4r            | cmp - .c4th_b5di
	# Plain c4 too: the generated code uses nothing above EXIT, which is
	# why EMIT compiles to PRTF rather than to c4m's PUTC.
	./c4 c4l.c .c4th_b5d.c4r | sed '/^exit(/d' | cmp - .c4th_b5di
	# Independent confirmation that the FORMAT is right, not just the
	# behaviour: c4r.lisp decodes the image and re-encodes it byte for
	# byte. A writer that is subtly wrong passes the behaviour checks on
	# a forgiving loader and fails this.
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp .c4th_b5d.c4r | grep -q "roundtrip identical"
	# And it must survive the optimizer, and the fused opcodes.
	./c4sp src/c4sp/lisp/c4opt-run.lisp .c4th_b5d.c4r .c4th_b5do.c4r > /dev/null
	$(C4M) load-c4r.c -- .c4th_b5do.c4r | cmp - .c4th_b5di
	./c4sp src/c4sp/lisp/c4opt-run.lisp -mfuse .c4th_b5d.c4r .c4th_b5df.c4r > /dev/null
	./c4mp .c4th_b5df.c4r | cmp - .c4th_b5di
	# And the differential fuzzer, both ways. b5.f is the cases somebody
	# thought of; this is two thousand nobody did -- random balanced
	# definitions with branches, IF/ELSE and counted loops, each run on
	# the threaded engine and then compiled and CALLED, with the answers
	# required to agree. The seed is fixed, so a failure reproduces.
	# Verified to actually catch things: a one-character change to -ROT's
	# permutation turns this from 0 mismatches into 8.
	$(C4M) load-c4r.c -- c4th.c4r src/c4th/forth/core.f src/c4th/forth/asm.f src/c4th/forth/native.f src/c4th/tests/fuzz.f | cmp - src/c4th/tests/expected/fuzz.txt
	$(C4M) load-c4r.c -- c4th.c4r src/c4th/forth/core.f src/c4th/forth/asm.f src/c4th/forth/native.f src/c4th/tests/nopc.f src/c4th/tests/fuzz.f | cmp - src/c4th/tests/expected/fuzz.txt
	# B5d.5: the fixed point. src/c4th/forth/self.f is a Forth
	# compiler written in the Forth it compiles -- see
	# docs/c4th-selfhost.md for why that, and not native.f, is what
	# a fixed point needs.
	#
	# First the ordinary differential, as at B5d: c4th runs the
	# program, self.f compiles it, and the image must print the same
	# thing on every machine that can run it. Plain c4 included --
	# self.f emits nothing above EXIT.
	./c4th src/c4th/forth/core.f src/c4th/tests/self1.f -e 'MAIN' > .c4th_self1i
	./c4th src/c4th/forth/core.f src/c4th/forth/self.f \
	       -e 'S" src/c4th/tests/self1.f" CSTR SRCP ! MAIN' > .c4th_self1.c4r
	$(C4M) load-c4r.c -- .c4th_self1.c4r | cmp - .c4th_self1i
	./c4mp .c4th_self1.c4r               | cmp - .c4th_self1i
	$(OISC4) .c4th_self1.c4r             | cmp - .c4th_self1i
	./c4 c4l.c .c4th_self1.c4r | sed '/^exit(/d' | cmp - .c4th_self1i
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp .c4th_self1.c4r | grep -q "roundtrip identical"
	# Then the compiler on itself. gen1 is self.f compiled by self.f
	# running on c4th's threaded engine -- the engine that passes the
	# Forth-2012 CORE suite; gen2 is self.f compiled by gen1; gen3 by
	# gen2. All three are the same bytes, which is the claim: the
	# hosted compiler and the compiled compiler agree, and the
	# compiled one agrees with itself.
	./c4th src/c4th/forth/core.f src/c4th/forth/self.f -e 'MAIN' > .c4th_gen1.c4r
	$(C4M) load-c4r.c -- .c4th_gen1.c4r > .c4th_gen2.bin
	cmp .c4th_gen1.c4r .c4th_gen2.bin
	$(C4M) load-c4r.c -- .c4th_gen2.bin > .c4th_gen3.bin
	cmp .c4th_gen1.c4r .c4th_gen3.bin
	# and the same fixed point on the other three machines. Plain c4
	# adds its own exit() line after the image, so compare the image's
	# worth of bytes rather than filtering a binary through sed.
	./c4mp .c4th_gen2.bin    | cmp - .c4th_gen1.c4r
	$(OISC4) .c4th_gen2.bin  | cmp - .c4th_gen1.c4r
	./c4 c4l.c .c4th_gen2.bin | head -c `stat -c %s .c4th_gen1.c4r` | cmp - .c4th_gen1.c4r
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp .c4th_gen1.c4r | grep -q "roundtrip identical"
	# And it must survive the optimizer: c4opt rewrites the compiler,
	# and the rewritten compiler still emits the same image it did.
	./c4sp src/c4sp/lisp/c4opt-run.lisp .c4th_gen1.c4r .c4th_gen1o.c4r > /dev/null
	$(C4M) load-c4r.c -- .c4th_gen1o.c4r | cmp - .c4th_gen1.c4r
	rm -f .c4th_core .c4th_fact.c4r .c4th_ccasm .c4th_b5 .c4th_fused
	rm -f .c4th_b5di .c4th_b5d.c4r .c4th_b5do.c4r .c4th_b5df.c4r
	rm -f .c4th_self1i .c4th_self1.c4r
	rm -f .c4th_gen1.c4r .c4th_gen1o.c4r .c4th_gen2.bin .c4th_gen3.bin
	@echo "test-c4th: OK"

# B6: c4th on the operating systems. docs/c4th-design.md.
#
# c4th needs nothing from any of them and gets nothing: it asks for
# open, read, close, malloc and printf, which is what it would ask a
# bare VM for, and each kernel answers in its own way -- C4KE and C4IX
# by trapping the syscall out of a protected task and emulating it,
# C4DOS by getting out of the way. The same c4th.c4r runs on all three.
C4TH_OS_PROG := src/c4th/forth/core.f src/c4th/tests/self1.f src/c4th/tests/run-main.f
# The kernels announce themselves; only the program's own output is
# being compared.
C4TH_KE_FILTER := grep -v "^c4ke\|^lc4r\|^ \|^$$\|Have a nice"
C4TH_IX_MASK := sed -E -e 's/[0-9]+ cycles/N cycles/g' -e 's/info 0x[0-9a-f]+/info 0xHH/'

c4dos-c4th: c4dos-clock.c4r c4th.c4r $(SRCS)/c4dos/fs/CONFIG.SYS
	@mkdir -p c4dos-c4th
	@cp $(SRCS)/c4dos/fs/CONFIG.SYS c4dos-c4th/config.sys
	@printf 'ECHO OFF\n' > c4dos-c4th/autoexec.bat
	@cp c4th.c4r c4dos-c4th/
	@cp src/c4th/forth/core.f src/c4th/tests/self1.f src/c4th/tests/run-main.f c4dos-c4th/
	@cd c4dos-c4th && ls > c4dos.dir
	@echo "c4dos-c4th: ready -- boot it and type RUN c4th.c4r core.f self1.f run-main.f"

test-c4th-os: c4th c4th.c4r $(C4M) c4ix.c4r c4ix-sh.c4r c4ke.c4r c4dos-clock.c4r c4dos-c4th
	# The host is the oracle, as everywhere else in this suite.
	./c4th $(C4TH_OS_PROG) > .c4th_os_ref
	# C4KE: a protected task, syscalls trapped and emulated.
	$(C4M) load-c4r.c -- c4ke.c4r c4th.c4r $(C4TH_OS_PROG) | $(C4TH_KE_FILTER) | cmp - .c4th_os_ref
	# C4DOS: no protection and no processes -- RUN loads the image and
	# calls it, and the working directory IS the disk, so the source
	# files sit beside the interpreter.
	cd c4dos-c4th && printf 'RUN c4th.c4r core.f self1.f run-main.f\nEXIT\n' | \
		$(CURDIR)/$(C4M) $(CURDIR)/load-c4r.c -- $(CURDIR)/c4dos-clock.c4r \
		| sed -e '1,/^A>ECHO OFF/d' -e '$$d' -e 's/^A>//' | cmp - $(CURDIR)/.c4th_os_ref
	# C4IX: driven by C4IX's own shell, so every line is also a test of
	# spawn, argv, the fd layer and redirection. The session compiles a
	# program, runs what it built, then compiles the COMPILER and runs
	# that -- the B5d.5 fixed point, inside the kernel. The image comes
	# back on standard output between two halves of a transcript, so
	# the two are checked separately.
	$(C4M) load-c4r.c -- c4ix.c4r c4ix-sh.c4r src/c4th/tests/c4ix.sh > .c4th_ix.out
	head -c `grep -abo -m1 C4R .c4th_ix.out | head -1 | cut -d: -f1` .c4th_ix.out \
		| $(C4TH_IX_MASK) | cmp - src/c4th/tests/expected/c4ix.txt
	./c4th src/c4th/forth/core.f src/c4th/forth/self.f -e 'MAIN' > .c4th_gen1.c4r
	tail -c +`expr \`grep -abo -m1 C4R .c4th_ix.out | head -1 | cut -d: -f1\` + 1` .c4th_ix.out \
		| head -c `stat -c %s .c4th_gen1.c4r` | cmp - .c4th_gen1.c4r
	grep -q "shutdown complete" .c4th_ix.out
	rm -f .c4th_os_ref .c4th_ix.out .c4th_gen1.c4r
	@echo "test-c4th-os: OK"

# c4fc: the C99 compiler in Forth, and the DSL it is written in.
# docs/c4fc-design.md. Nothing of the compiler exists yet; what this
# pins is the vocabulary underneath it -- the Forth-2012 word sets
# c4th's kernel lacks (ext.f), named locals (locals.f), and the
# compiler substrate (dsl.f): arena, vectors, tagged nodes and
# generics.
#
# The last section of the test is the design's whole claim, run rather
# than asserted: a new construct is one NODE: line and one :M per
# phase, with nothing above it edited.
C4FC_LIB := src/c4th/forth/core.f src/c4th/forth/ext.f \
            src/c4th/forth/locals.f src/c4th/forth/dos.f \
            src/c4fc/dsl.f src/c4fc/lex.f
C4FC_ALL := $(C4FC_LIB) src/c4fc/pp.f src/c4fc/ast.f src/c4fc/types.f \
            src/c4fc/emit.f src/c4fc/tree.f src/c4fc/gen.f src/c4fc/parse.f \
            src/c4fc/opt.f src/c4fc/c4fc.f
C4FC_SPIKE := src/tests/hello.c src/c4fc/tests/spike1.c src/c4fc/tests/spike2.c \
              src/c4fc/tests/spike3.c src/c4fc/tests/spike4.c \
              src/c4fc/tests/spike5.c src/c4fc/tests/spike6.c \
              src/c4fc/tests/spike7.c src/c4fc/tests/spike8.c

# F2, the lexer. The oracle is c4lc's own, three ways: its golden dump
# of the sample that carries every token kind and quirk, the same with
# -conforming escapes, and then a sweep of real sources compared against
# freshly generated c4lc output -- which is the check that matters,
# because the sample is 70 lines and c4m.c is thirteen thousand tokens.
#
# head -n -1 strips the "nil" c4sp's REPL prints after the dump.
C4FC_LEX_SWEEP := c4.c c4m.c c4l.c load-c4r.c src/c4th/c4th.c \
                  src/c4cc/asm-c4r.c src/c4dos/c4dos.c src/c4ix/vfs.c \
                  src/c4ix/sched.c src/c4or1k/cpu.c src/tests/mandel.c \
                  src/tests/tests.c

# F3, the preprocessor sweep: <source>:<-I directory>. The twelve C4IX
# modules are the bar the design named; the rest are there because a
# preprocessor that only ever sees one project's headers has not been
# tested. src/c4dos/c4dos.c is deliberately absent -- it is built by gcc's
# cpp and c4cc, and its `#define int long long` macro-defines a KEYWORD,
# which neither c4lc nor c4fc models.
C4FC_PP_SWEEP = $(patsubst %,src/c4ix/%.c:src/c4ix,$(C4IX_MODS)) \
                 src/c4ix/lib/libc4ix.c:src/c4ix/include \
                 src/c4ix/user/sh.c:src/c4ix/include \
                 src/c4or1k/cpu.c:src/c4or1k src/c4mp/vm.c:src/c4mp \
                 src/c4cc/c4cc.c:. src/c4ke/c4ke.c:src/c4ke \
                 src/c4th/c4th.c:src/c4th/include src/c4sp/c4sp.c:src/c4sp/include \
                 load-c4r.c:.

# F9, the codegen sweep: whole programs c4fc preprocesses AND compiles
# itself, against c4lc given the same source through gcc -E. factorial,
# test_malloc, test-oisc and test_printf are absent because c4lc cannot
# compile them by this route (they have their own paths in test-c4lc),
# so there would be no oracle.
C4FC_GEN_SWEEP := hello multifun puts reverse test_basic test_continue \
                  test_coop_switch test_globals test-order test-ptrs \
                  test_static test_switch tests c4lc_l2 global \
                  test_gcscan vararg2 test_vprintf c4_jailbreak

test-c4fc: c4th c4th.c4r $(C4M) c4sp c4mp c4 c4l.c
	./c4th $(C4FC_LIB) src/c4fc/tests/dsl.f | cmp - src/c4fc/tests/expected/dsl.txt
	$(C4M) load-c4r.c -- c4th.c4r $(C4FC_LIB) src/c4fc/tests/dsl.f | cmp - src/c4fc/tests/expected/dsl.txt
	./c4th $(C4FC_LIB) -e ': GO 4194304 ARENA-INIT S" src/tests/c4lc_lex_sample.c" LEX-FILE DUMP-TOKENS ; GO' > .c4fc_lex.txt
	head -n -1 src/c4sp/tests/expected/c4lc-tokens.txt | cmp - .c4fc_lex.txt
	./c4th $(C4FC_LIB) -e ': GO 4194304 ARENA-INIT 1 CONFORMING ! S" src/tests/c4lc_lex_sample.c" LEX-FILE DUMP-TOKENS ; GO' > .c4fc_lex.txt
	head -n -1 src/c4sp/tests/expected/c4lc-tokens-conforming.txt | cmp - .c4fc_lex.txt
	./c4th $(C4FC_LIB) -e ': GO 33554432 ARENA-INIT S" src/c4cc/c4cc.c" LEX-FILE COUNT-TOKENS ; GO' | grep -q "^tokens 15125$$"
	@for f in $(C4FC_LEX_SWEEP); do \
	   ./c4sp src/c4sp/lisp/c4lc-tokens.lisp $$f > .c4fc_a.txt 2>&1; \
	   ./c4th $(C4FC_LIB) -e ": GO 67108864 ARENA-INIT S\" $$f\" LEX-FILE DUMP-TOKENS ; GO" > .c4fc_b.txt 2>&1; \
	   head -n -1 .c4fc_a.txt | cmp -s - .c4fc_b.txt \
	     || { echo "test-c4fc: the lexer differs from c4lc on $$f"; exit 1; }; \
	   echo "  lex ok: $$f"; \
	done
	# Whole programs compiled to a .c4r and required to be
	# BYTE-IDENTICAL to c4lc's -- the bar the whole ladder is verified
	# against. spike1 and spike2 are the straight-line slice that
	# answered whether the bar is reachable at all; spike3 is F4's
	# control flow and spike4 its switch, which is a jump table in the
	# data segment reached through JMPA; spike5 and spike6 are F5/F6 --
	# pointer scaling, arrays, structs and enums; spike7 and spike8 are
	# F7 -- storage classes, prototypes, initialisers, constant
	# expressions, variadic functions and the constructor lists.
	@for f in $(C4FC_SPIKE); do \
	   ./c4sp src/c4sp/lisp/c4lc.lisp $$f .c4fc_lc.c4r > /dev/null 2>&1; \
	   ./c4th $(C4FC_ALL) -e ": GO S\" $$f\" C4FC ; GO" > .c4fc_fc.c4r; \
	   cmp .c4fc_lc.c4r .c4fc_fc.c4r \
	     || { echo "test-c4fc: $$f differs from c4lc"; exit 1; }; \
	   echo "  gen ok: $$f"; \
	done
	# F8 and L6: the optimizer, both halves. -O is the AST passes
	# (constant folding, dead function elimination) followed by the six
	# peephole passes, and the bar is c4lc -O itself, byte for byte.
	#
	# That bar is stronger than the one F8 shipped with, and the reason
	# it is now reachable is worth writing down: F8 compared against
	# c4opt-run.lisp, which DECODES a finished image, optimises and
	# re-encodes -- and leaves the pre-optimisation address in a patched
	# operand's code word. c4lc never goes through that path; it
	# optimises its own labelled IR and encodes once, so both the word
	# and the patch get the final address, which is what c4fc does too.
	# The "identical once loaded" fallback was an artefact of the tool
	# being compared against, not of the format.
	@for f in $(C4FC_SPIKE); do \
	   ./c4sp src/c4sp/lisp/c4lc.lisp -O $$f .c4fc_o2.c4r >/dev/null 2>&1; \
	   ./c4th $(C4FC_ALL) -e ": GO S\" $$f\" C4FC ; GO" > .c4fc_o0.c4r; \
	   ./c4th $(C4FC_ALL) -e ": GO 1 OPTIMIZE ! S\" $$f\" C4FC ; GO" > .c4fc_o1.c4r; \
	   cmp -s .c4fc_o1.c4r .c4fc_o2.c4r \
	     || { echo "test-c4fc: -O differs from c4lc -O on $$f"; exit 1; }; \
	   ( $(C4M) load-c4r.c -- .c4fc_o0.c4r; echo "exit $$?" ) > .c4fc_r0 2>&1; \
	   ( $(C4M) load-c4r.c -- .c4fc_o1.c4r; echo "exit $$?" ) > .c4fc_r1 2>&1; \
	   cmp -s .c4fc_r0 .c4fc_r1 \
	     || { echo "test-c4fc: -O changed what $$f does"; exit 1; }; \
	   echo "  opt ok: $$f"; \
	done
	# F3: the preprocessor. Three bars, weakest first.
	#
	# One, the feature battery -- the same file c4lc's own L9 test uses,
	# compared token for token against c4lc's preprocessor. Tokens and
	# not text: gcc -E emits `# 12 "file"` markers and c4fc consumes
	# them, so the two can never agree on a line number and must agree
	# on everything else.
	./c4sp src/c4sp/lisp/c4lc-ppdump.lisp $(TESTS)/c4lc_pp.c > .c4fc_a.txt
	./c4th $(C4FC_LIB) src/c4fc/pp.f -e ': GO 67108864 ARENA-INIT PP-RESET S" $(TESTS)/c4lc_pp.c" PP-FILE DUMP-PPTOKENS ; GO' > .c4fc_b.txt
	head -n -1 .c4fc_a.txt | cmp - .c4fc_b.txt
	# Two, the bar the design asked for: real modules, preprocessed by
	# c4fc, against the SAME modules preprocessed by gcc -E. Every one of
	# the twelve C4IX modules plus a spread of the rest of the tree.
	# __GNUC__ is defined on the c4fc side because gcc defines it on its
	# own side and c4.h branches on it; the other four are what $(PREPROC)
	# passes.
	@for spec in $(C4FC_PP_SWEEP); do \
	   f=$${spec%%:*}; inc=$${spec##*:}; \
	   gcc -E -Iinclude -I. -I$$inc -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -C $$f > .c4fc_pp.c 2>/dev/null || exit 1; \
	   ./c4th $(C4FC_LIB) src/c4fc/pp.f -e ": GO 200000000 ARENA-INIT S\" .c4fc_pp.c\" LEX-FILE DUMP-PPTOKENS ; GO" > .c4fc_a.txt 2>&1; \
	   ./c4th $(C4FC_LIB) src/c4fc/pp.f -e ": GO 200000000 ARENA-INIT PP-RESET S\" include\" PP-PATH S\" .\" PP-PATH S\" $$inc\" PP-PATH S\" C4CC=1\" PP-DEFINE S\" __c4__=1\" PP-DEFINE S\" __C4CC__=1\" PP-DEFINE S\" __c4cc__=1\" PP-DEFINE S\" __GNUC__=1\" PP-DEFINE S\" $$f\" PP-FILE DUMP-PPTOKENS ; GO" > .c4fc_b.txt 2>&1; \
	   cmp -s .c4fc_a.txt .c4fc_b.txt \
	     || { echo "test-c4fc: the preprocessor differs from gcc -E on $$f"; exit 1; }; \
	   echo "  pp ok: $$f"; \
	done
	# Three, the strongest: a program that USES the preprocessor,
	# compiled all the way to an image, byte-identical to c4lc -P's --
	# and then run, because an image that matches and does the wrong
	# thing would mean both compilers are wrong the same way.
	./c4sp src/c4sp/lisp/c4lc.lisp -P -I src/c4fc/tests src/c4fc/tests/spike9.c .c4fc_lc.c4r > /dev/null
	./c4th $(C4FC_ALL) -e ': GO C4FC-INIT -P S" src/c4fc/tests" -I S" src/c4fc/tests/spike9.c" C4FC ; GO' > .c4fc_fc.c4r
	cmp .c4fc_lc.c4r .c4fc_fc.c4r
	$(C4M) load-c4r.c -- .c4fc_fc.c4r | cmp - src/c4fc/tests/expected/spike9.txt
	# F9, part one: whole programs compiled by c4fc THROUGH ITS OWN
	# PREPROCESSOR, byte-identical to c4lc handed the same source via
	# gcc -E. This is the differential that found local initialisers,
	# indirect calls, casts-as-types, sizeof(array), the comma operator,
	# `int *a, *b`, a bare `return;` and `char *s = "..."` -- every one
	# of them a construct the spikes never wrote and every real header
	# does.
	@for t in $(C4FC_GEN_SWEEP); do \
	   gcc -E -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -C \
	      $(TESTS)/$$t.c > .c4fc_pp.c 2>/dev/null; \
	   ./c4sp src/c4sp/lisp/c4lc.lisp .c4fc_pp.c .c4fc_lc.c4r >/dev/null 2>&1 \
	     || { echo "test-c4fc: c4lc could not compile $$t"; exit 1; }; \
	   ./c4th $(C4FC_ALL) -e ": GO C4FC-INIT -P S\" include\" -I S\" .\" -I S\" C4CC=1\" -D S\" __c4__=1\" -D S\" __C4CC__=1\" -D S\" __c4cc__=1\" -D S\" __GNUC__=1\" -D S\" $(TESTS)/$$t.c\" C4FC ; GO" > .c4fc_fc.c4r 2>&1; \
	   cmp -s .c4fc_lc.c4r .c4fc_fc.c4r \
	     || { echo "test-c4fc: $$t differs from c4lc (-P)"; exit 1; }; \
	   ./c4sp src/c4sp/lisp/c4lc.lisp -O .c4fc_pp.c .c4fc_lc.c4r >/dev/null 2>&1 \
	     || { echo "test-c4fc: c4lc -O could not compile $$t"; exit 1; }; \
	   ./c4th $(C4FC_ALL) -e ": GO 1 OPTIMIZE ! C4FC-INIT -P S\" include\" -I S\" .\" -I S\" C4CC=1\" -D S\" __c4__=1\" -D S\" __C4CC__=1\" -D S\" __c4cc__=1\" -D S\" __GNUC__=1\" -D S\" $(TESTS)/$$t.c\" C4FC ; GO" > .c4fc_fc.c4r 2>&1; \
	   cmp -s .c4fc_lc.c4r .c4fc_fc.c4r \
	     || { echo "test-c4fc: $$t differs from c4lc -O"; exit 1; }; \
	   echo "  gen -P ok (-O and not): $$t"; \
	done
	# F9, the closing loop: c4fc compiles c4th.c -- preprocessor and all,
	# thirteen headers and thirty thousand tokens -- to an image
	# BYTE-IDENTICAL to c4lc's, and the c4th that comes out passes the
	# Forth-2012 CORE suite with the same transcript the pinned golden
	# holds. The Forth compiles the C compiler that compiles the Forth.
	$(PREPROC) -I src/c4th/include src/c4th/c4th.c > .c4fc_pp.c
	./c4sp src/c4sp/lisp/c4lc.lisp .c4fc_pp.c .c4fc_lc.c4r > /dev/null
	./c4th $(C4FC_ALL) -e ': GO C4FC-INIT -P S" include" -I S" ." -I S" src/c4th/include" -I S" C4CC=1" -D S" __c4__=1" -D S" __C4CC__=1" -D S" __c4cc__=1" -D S" __GNUC__=1" -D S" src/c4th/c4th.c" C4FC ; GO' > .c4fc_fc.c4r
	cmp .c4fc_lc.c4r .c4fc_fc.c4r
	echo c4th | $(C4M) load-c4r.c -- .c4fc_fc.c4r src/c4th/forth/core.f src/c4th/tests/tester.fr src/c4th/tests/core.fr | cmp - src/c4th/tests/expected/core-64.txt
	# ... and again with -O, where the tree passes take about a fifth of
	# the image away and the suite must still pass.
	./c4sp src/c4sp/lisp/c4lc.lisp -O .c4fc_pp.c .c4fc_lc.c4r > /dev/null
	./c4th $(C4FC_ALL) -e ': GO 1 OPTIMIZE ! C4FC-INIT -P S" include" -I S" ." -I S" src/c4th/include" -I S" C4CC=1" -D S" __c4__=1" -D S" __C4CC__=1" -D S" __c4cc__=1" -D S" __GNUC__=1" -D S" src/c4th/c4th.c" C4FC ; GO' > .c4fc_fc.c4r
	cmp .c4fc_lc.c4r .c4fc_fc.c4r
	echo c4th | $(C4M) load-c4r.c -- .c4fc_fc.c4r src/c4th/forth/core.f src/c4th/tests/tester.fr src/c4th/tests/core.fr | cmp - src/c4th/tests/expected/core-64.txt
	# L6 on two whole operating systems. C4DOS and C4KE are the largest
	# things in the tree c4fc can compile as one unit, and the second is
	# a kernel: an image that is byte-identical AND boots is a stronger
	# statement than either alone. C4KE also goes through gcc -E on the
	# c4lc side because c4lc's own preprocessor cannot read it -- a
	# function-like macro named without an argument list is an error
	# there and stands for itself here, which is what C says.
	$(PREPROC) -I src/c4ke src/c4ke/c4ke.c > .c4fc_pp.c
	./c4sp src/c4sp/lisp/c4lc.lisp -O .c4fc_pp.c .c4fc_lc.c4r > /dev/null
	./c4th $(C4FC_ALL) -e ': GO 1 OPTIMIZE ! C4FC-INIT -P S" include" -I S" ." -I S" src/c4ke" -I S" C4CC=1" -D S" __c4__=1" -D S" __C4CC__=1" -D S" __c4cc__=1" -D S" src/c4ke/c4ke.c" C4FC ; GO' > .c4fc_fc.c4r
	cmp .c4fc_lc.c4r .c4fc_fc.c4r
	$(C4M) load-c4r.c -- .c4fc_fc.c4r test_basic 2>&1 | grep -q "clean shutdown"
	$(C4M) load-c4r.c -- .c4fc_fc.c4r test_basic 2>&1 | grep -q "^  5"
	./c4sp src/c4sp/lisp/c4lc.lisp -O -P -I include -I . -I src/c4dos -D C4CC=1 -D __c4cc__=1 src/c4dos/c4dos.c .c4fc_lc.c4r > /dev/null
	./c4th $(C4FC_ALL) -e ': GO 1 OPTIMIZE ! C4FC-INIT -P S" include" -I S" ." -I S" src/c4dos" -I S" C4CC=1" -D S" __c4cc__=1" -D S" src/c4dos/c4dos.c" C4FC ; GO' > .c4fc_fc.c4r
	cmp .c4fc_lc.c4r .c4fc_fc.c4r
	# -mcisc and -mfuse, the opcodes c4m does not have. -mcisc is a
	# codegen choice (LXI/SXI fold scale-add-load into one c4mp opcode
	# for var[expr] with an 8-byte scalar element type); -mfuse is a
	# peephole pass that runs ONCE after the fixpoint. Both are checked
	# the way everything else here is -- against c4lc, byte for byte --
	# and then against the machines: c4mp runs the fused image, and c4m
	# and plain c4 must NAME the opcode they lack rather than execute
	# rubbish.
	@for f in $(C4FC_SPIKE); do \
	   for m in "-mfuse" "-mcisc -O" "-mcisc -mfuse"; do \
	      ./c4sp src/c4sp/lisp/c4lc.lisp $$m $$f .c4fc_lc.c4r >/dev/null 2>&1; \
	      ./c4th $(C4FC_ALL) -e ": GO `echo $$m | sed 's/-O/1 OPTIMIZE !/'` S\" $$f\" C4FC ; GO" > .c4fc_fc.c4r 2>&1; \
	      cmp -s .c4fc_lc.c4r .c4fc_fc.c4r \
	        || { echo "test-c4fc: $$f differs from c4lc $$m"; exit 1; }; \
	   done; \
	   echo "  mcisc/mfuse ok: $$f"; \
	done
	./c4th $(C4FC_ALL) -e ': GO -mfuse -mcisc S" src/tests/tests.c" C4FC ; GO' > .c4fc_fc.c4r
	./c4mp .c4fc_fc.c4r | grep -q "tests succeeded"
	$(C4M) load-c4r.c -- .c4fc_fc.c4r 2>&1 | grep -q "is not an instruction this machine has"
	./c4 c4l.c .c4fc_fc.c4r 2>&1 | grep -q "which plain c4 does not have"
	# ... and the build that really uses -mcisc: three c4or1k modules as
	# objects, byte-identical to c4lc's.
	@for m in cpu mem virtio9p; do \
	   ./c4sp src/c4sp/lisp/c4lc.lisp -mcisc -O -c -I $(C4OR1K_SRC) $(C4OR1K_SRC)/$$m.c .c4fc_lc.c4o >/dev/null 2>&1; \
	   ./c4th $(C4FC_ALL) -e ": GO -mcisc 1 OPTIMIZE ! -c C4FC-INIT -P S\" include\" -I S\" .\" -I S\" $(C4OR1K_SRC)\" -I S\" C4CC=1\" -D S\" __c4__=1\" -D S\" __C4CC__=1\" -D S\" __c4cc__=1\" -D S\" $(C4OR1K_SRC)/$$m.c\" C4FC ; GO" > .c4fc_fc.c4o 2>&1; \
	   cmp -s .c4fc_lc.c4o .c4fc_fc.c4o \
	     || { echo "test-c4fc: c4or1k/$$m.c4o differs from c4lc -mcisc -O -c"; exit 1; }; \
	   echo "  mcisc obj ok: $$m"; \
	done
	# F10, object mode: the whole of C4IX. Twelve .c4o objects, each
	# byte-identical to the one c4lc -O -c writes, linked by c4rlink into
	# a kernel that must equal the committed image and then boot. An
	# object is where a compiler's bookkeeping shows: what it could not
	# resolve it has to NAME, in the order c4lc names them, and the
	# patches that reference those names carry a symbol id where a
	# whole-program image would carry -1 or -2.
	@for m in $(C4IX_MODS); do \
	   ./c4sp src/c4sp/lisp/c4lc.lisp -O -c -I $(C4IX_SRC) $(C4IX_SRC)/$$m.c .c4fc_lc.c4o >/dev/null 2>&1 \
	     || { echo "test-c4fc: c4lc could not compile $$m"; exit 1; }; \
	   ./c4th $(C4FC_ALL) -e ": GO 1 OPTIMIZE ! -c C4FC-INIT -P S\" include\" -I S\" .\" -I S\" $(C4IX_SRC)\" -I S\" C4CC=1\" -D S\" __c4__=1\" -D S\" __C4CC__=1\" -D S\" __c4cc__=1\" -D S\" $(C4IX_SRC)/$$m.c\" C4FC ; GO" > .c4fc_ix_$$m.c4o 2>&1; \
	   cmp -s .c4fc_lc.c4o .c4fc_ix_$$m.c4o \
	     || { echo "test-c4fc: $$m.c4o differs from c4lc -O -c"; exit 1; }; \
	   echo "  obj ok: $$m"; \
	done
	$(C4RLINK) $(patsubst %,.c4fc_ix_%.c4o,$(C4IX_MODS)) -o .c4fc_ix.c4r
	$(MAKE) c4ix.c4r
	cmp .c4fc_ix.c4r c4ix.c4r
	$(C4M) load-c4r.c -- .c4fc_ix.c4r --demo 2>&1 | grep -q "shutdown complete"
	@rm -f .c4fc_lc.c4o .c4fc_ix.c4r .c4fc_ix_*.c4o
	@rm -f .c4fc_lex.txt .c4fc_a.txt .c4fc_b.txt .c4fc_lc.c4r .c4fc_fc.c4r
	@rm -f .c4fc_o0.c4r .c4fc_o1.c4r .c4fc_o2.c4r .c4fc_r0 .c4fc_r1 .c4fc_pp.c
	@echo "test-c4fc: OK"

# A C4DOS floppy whose COMPILER IS c4fc. The existing build floppy uses
# cpp + c4cc, which is the pair that has always been able to do it; this
# one carries c4th32 and c4fc's Forth sources instead, plus c4rlink, so
# the machine can compile a unit to an object and link objects into a
# kernel -- the shape a real toolchain has and the one c4cc cannot do.
#
# The compiler is source rather than an image because c4th has no image
# save: its dictionary holds real machine addresses (docs/c4th-design.md)
# so a saved image is unrelocatable, and the VM has no write syscall to
# save one with in the first place. Loading the sources costs 19 s on
# c4bb, which is real and is not where the time goes -- see
# docs/c4dos-design.md.
C4FC_FORTH := src/c4th/forth/core.f src/c4th/forth/ext.f \
              src/c4th/forth/locals.f src/c4th/forth/dos.f \
              src/c4fc/dsl.f src/c4fc/lex.f src/c4fc/pp.f src/c4fc/ast.f \
              src/c4fc/types.f src/c4fc/emit.f src/c4fc/tree.f \
              src/c4fc/gen.f src/c4fc/parse.f src/c4fc/opt.f src/c4fc/c4fc.f

C4DOS_FC_DISK := c4dos-c4fc
$(C4DOS_FC_DISK): c4dos-clock.c4r c4th32.c4r $(C4R_C4RLINK) dostar.c4r \
                  c4ke-src.tar dosload.c4r $(C4FC_FORTH) \
                  $(SRCS)/c4dos/fs/CONFIG.SYS $(SRCS)/c4dos/fs/CC.BAT $(SRCS)/c4dos/fs/cc.f
	@mkdir -p $(C4DOS_FC_DISK)
	@sed 's/SIZE=[0-9]*/SIZE=16777216/' $(SRCS)/c4dos/fs/CONFIG.SYS > $(C4DOS_FC_DISK)/config.sys
	@# c4bb's UART emits each byte as it is written, so the classic
	@# tight A> prompt works here. Say so: DOS defaults to a prompt on
	@# its own line, because a host libc buffers a partial one.
	@echo 'DEVICE=CONSOLE.SYS FLUSH' >> $(C4DOS_FC_DISK)/config.sys
	@# And the drives. Same rule: the board has them at 0x13c/0x188,
	@# native c4m has ordinary memory there, so DOS is told rather than
	@# left to probe. Without this line the prompt is A> and stays A>.
	@echo 'DEVICE=DRIVES.SYS' >> $(C4DOS_FC_DISK)/config.sys
	@cp $(SRCS)/c4dos/fs/CC.BAT $(C4DOS_FC_DISK)/cc.bat
	@cp $(SRCS)/c4dos/fs/cc.f   $(C4DOS_FC_DISK)/cc.f
	@printf 'int main(){ printf("built by c4fc, inside the machine\\n"); return 0; }\n' > $(C4DOS_FC_DISK)/hello.c
	@cp c4th32.c4r $(C4DOS_FC_DISK)/c4th.c4r
	@cp $(C4R_C4RLINK) dostar.c4r dosload.c4r c4ke-src.tar $(C4DOS_FC_DISK)/
	@# ONE file, not sixteen: C4DOS's ARGVMAX is 16 tokens, so
	@# "RUN c4th.c4r <fifteen .f files>" is silently truncated and
	@# nothing happens. Concatenated in load order, which is also one
	@# open instead of fifteen on a machine where an open is not cheap.
	@cat $(C4FC_FORTH) > $(C4DOS_FC_DISK)/c4fc.f
	@cd $(C4DOS_FC_DISK) && ls > c4dos.dir
	@echo "c4dos-c4fc: ready -- RUN c4th.c4r ... or CC"

# The whole loop in one command: C4DOS boots, loads c4fc, compiles a C
# program, writes the image to its RAM disk through the API table, and
# runs what it just built. Three minutes, so it is not in `make test`.
test-c4dos-c4fc: c4dos32.c4r $(C4DOS_FC_DISK)
	printf 'RUN c4th.c4r c4fc.f cc.f\nRUN hello.c4r\n' \
	  | node src/c4bb/sim/cli.js -m 96 -d $(C4DOS_FC_DISK) c4dos32.c4r \
	  | tee .c4dos_fc.log | grep -q "built by c4fc, inside the machine"
	grep -q "^wrote the object" .c4dos_fc.log
	rm -f .c4dos_fc.log
	@echo "test-c4dos-c4fc: OK"

run-c4dos-c4fc: c4dos32.c4r $(C4DOS_FC_DISK)
	node src/c4bb/sim/cli.js -m 96 -i -d $(C4DOS_FC_DISK) c4dos32.c4r

# c4th for the breadboard. c4bb is a 32-bit machine, so the image has to
# be built by a 32-bit compiler -- c4lc under c4sp32, the same route
# c4ke32.c4r takes. c4lc's own preprocessor cannot read c4th.c (a
# function-like macro named without an argument list is an error there),
# so this one goes through gcc -E like the c4cc-built c4th.c4r does.
#
# What it is FOR: c4fc runs on c4th, so a 32-bit c4th is a C compiler on
# the breadboard. It compiles a real C4IX module there, byte-identical to
# c4lc's object -- see docs/c4fc-design.md. Not a routine test: it takes
# five and a half minutes on the board.
c4th32.c4r: c4sp32 $(C4LC_LISP) $(C4TH_SRCS)
	$(PREPROC) -I src/c4th/include src/c4th/c4th.c > .c4th32_pp.c
	./c4sp32 -R src/c4sp/lisp/c4lc.lisp -O .c4th32_pp.c $@ > /dev/null
	rm -f .c4th32_pp.c

# The cheap half of that: c4th itself on the breadboard.
test-c4th-bb: c4th32.c4r
	node src/c4bb/sim/cli.js c4th32.c4r -e ': SQ DUP * ; 7 SQ . CR' | grep -q "^49"
	@echo "test-c4th-bb: OK"

# c4tui: the character-cell UI library for C4DOS (docs/c4tui-design.md).
# The demo is also the test -- it is driven by a fixed key script and its
# output pinned, which checks the drawing AND the damage tracking: the
# golden records that a dialog costs 417 bytes where the first frame
# costs 2521. A change that repainted everything would still LOOK right
# and would show up here immediately.
c4tui-demo.c4r: c4sp $(C4LC_LISP) src/c4tui/c4tui.c src/c4tui/demo.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O -conforming -P -I src/c4tui \
		src/c4tui/demo.c $@ > /dev/null
c4tui-demo32.c4r: c4sp32 $(C4LC_LISP) src/c4tui/c4tui.c src/c4tui/demo.c
	./c4sp32 -R src/c4sp/lisp/c4lc.lisp -O -conforming -P -I src/c4tui \
		src/c4tui/demo.c $@ > /dev/null

test-c4tui: $(C4M) c4tui-demo.c4r c4tui-demo32.c4r
	printf '\033OQ\r\033' > .c4tui_keys
	$(C4M) load-c4r.c -- c4tui-demo.c4r < .c4tui_keys | cmp - src/c4tui/tests/expected/demo.txt
	# The same program on the breadboard, byte for byte -- the library
	# emits nothing that depends on the host.
	node src/c4bb/sim/cli.js c4tui-demo32.c4r < .c4tui_keys | cmp - src/c4tui/tests/expected/demo.txt
	# and the flush really is incremental
	$(C4M) load-c4r.c -- c4tui-demo.c4r < .c4tui_keys | awk 'NR==2 { exit !(length($$0) < 600) }'
	rm -f .c4tui_keys
	@echo "test-c4tui: OK"

# The fused opcodes (docs/fused-opcodes.md), end to end: take a real
# image, run c4opt's fuse pass over it, and require that it behaves
# identically on the hosts that have them -- and is REFUSED by the ones
# that do not.
#
# The refusal matters as much as the acceptance. Neither c4.c nor c4m.c
# is given these opcodes, so plain c4 must name the one it lacks and c4m
# must trap it, rather than either executing rubbish; before the tables
# were mirrored, c4l read past the end of its own name string to find
# out what to say.
test-fuse: c4sp c4cc $(C4M) c4mp $(OISC4) c4l.c
	./c4cc -o .fuse_t.c4r src/tests/tests.c > /dev/null
	./c4sp src/c4sp/lisp/c4opt-run.lisp -mfuse .fuse_t.c4r .fuse_tf.c4r > /dev/null
	$(C4M) load-c4r.c -- .fuse_t.c4r  | grep -q "tests succeeded"
	./c4mp .fuse_tf.c4r               | grep -q "tests succeeded"
	$(OISC4) .fuse_tf.c4r             | grep -q "tests succeeded"
	# And the two machines that do NOT have them must say so rather
	# than execute rubbish: c4m traps the opcode, plain c4 names it.
	$(C4M) load-c4r.c -- .fuse_tf.c4r 2>&1 | grep -q "is not an instruction this machine has"
	./c4 c4l.c .fuse_tf.c4r 2>&1 | grep -q "which plain c4 does not have"
	./c4 c4l.c .fuse_t.c4r  | grep -q "tests succeeded"
	rm -f .fuse_t.c4r .fuse_tf.c4r
	@echo "test-fuse: OK"

c4sp: $(C4SP_SRCS)
	gcc $(EXTRA_CC) -O2 -fwrapv -fno-omit-frame-pointer -g -Iinclude -I. -o c4sp src/c4sp/c4sp.c
# Built by c4lc -O, not c4cc: measurably smaller and never slower
# (docs/c4lc-design.md 11 measures -18.1% instructions on this image),
# and this is the c4sp that every hosted run uses -- under c4m, inside
# C4KE, and on c4bb -- so it is the one whose size and speed are felt.
c4sp.c4r: c4sp $(C4LC_LISP) $(C4SP_SRCS)
	$(PREPROC) -DC4SP_DOS=1 src/c4sp/c4sp.c > .c4sp_lcpp.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O .c4sp_lcpp.c c4sp.c4r > /dev/null
	rm -f .c4sp_lcpp.c
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
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp test_float.c4r | grep -q "roundtrip identical"
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp c4sp.c4r | grep -q "roundtrip identical"
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
	./c4sp src/c4sp/lisp/c4opt-run.lisp factorial.c4r .c4sp_opt.c4r
	./c4m load-c4r.c -- $(C4KE_C4R) factorial.c4r 2>&1 | grep -v "^c4ke\|^lc4r\|stacktrace\|Have a nice" > .c4sp_opt_a
	./c4m load-c4r.c -- $(C4KE_C4R) .c4sp_opt.c4r 2>&1 | grep -v "^c4ke\|^lc4r\|stacktrace\|Have a nice" > .c4sp_opt_b
	cmp .c4sp_opt_a .c4sp_opt_b
	./c4sp src/c4sp/lisp/c4opt-run.lisp $(C4R_C4CC) .c4sp_opt_cc.c4r
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
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O .c4lc_klc.c c4ke-lc.c4r
	rm -f .c4lc_klc.c
c4sp-lc.c4r: c4sp $(C4LC_LISP) $(C4SP_SRCS)
	$(PREPROC) -DC4SP_DOS=1 src/c4sp/c4sp.c > .c4lc_klc.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O .c4lc_klc.c c4sp-lc.c4r
	rm -f .c4lc_klc.c
c4m-lc.c4r: c4sp $(C4LC_LISP) c4m.c
	$(PREPROC) c4m.c > .c4lc_klc.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O .c4lc_klc.c c4m-lc.c4r
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
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O .c4lc_o4.c oisc4-lc.c4r
	rm -f .c4lc_o4.c
test-oisc4: $(OISC4) $(C4M) c4.c4r c4sp.c4r $(TESTS_C4R)
	bash src/oisc4/test-oisc4.sh

# C4BB: the breadboard computer (docs/c4bb-design.md). A JS-simulated
# microcoded 32-bit hardware implementation of the c4m instruction
# set; verified against a 32-bit native build of c4m by test-c4bb.
#
# c4sp32 makes c4lc a 32-bit compiler: c4lc's word size follows the
# host running it (g:WORD in c4lc-gen.lisp), so the same Lisp emits
# 32-bit images under c4sp32 and byte-identical 64-bit images under
# c4sp. The c4bb firmware builds with c4lc; the test corpus builds
# with c4cc32 so the parity suite exercises both compilers.
c4cc32: $(C4CC_SRCS)
	gcc -m32 $(NATIVE_CC_OPTS) src/c4cc/asm-c4r.c -o c4cc32 -lm
c4m32: c4m.c c4m_float.c
	gcc -m32 $(NATIVE_CC_OPTS) c4m.c c4m_float.c -o c4m32 -lm
c4sp32: $(C4SP_SRCS)
	gcc -m32 $(EXTRA_CC) -O2 -fwrapv -fno-omit-frame-pointer -g -Iinclude -I. -o c4sp32 src/c4sp/c4sp.c
# 32-bit .c4r builds of the toolchain, for the c4bb disk: these are the
# tools a system on that machine has to reach for, so they have to be
# images the machine can load, not host binaries.
c4sp32.c4r: c4sp32 $(C4LC_LISP) $(C4SP_SRCS)
	$(PREPROC) -DC4SP_DOS=1 src/c4sp/c4sp.c > .c4sp32_lcpp.c
	./c4sp32 -R src/c4sp/lisp/c4lc.lisp -O .c4sp32_lcpp.c c4sp32.c4r > /dev/null
	rm -f .c4sp32_lcpp.c
cpp32.c4r: c4cc32 include/c4dos.h $(SRCS)/c4dos/cpp.c
	./c4cc32 -o cpp32.c4r include/c4dos.h $(SRCS)/c4dos/cpp.c > /dev/null

# c4cc as a 32-bit image. The board's C compiler: docs/compiler-on-the-board.md
# measures it at 124M instructions for the whole C4KE kernel, against 85.5
# BILLION for c4fc, which is why the build floppy carries this and not that.
c4cc32.c4r: c4cc32 $(C4R_C4CC_SRCS)
	./c4cc32 -o $@ $(C4R_C4CC_SRCS) > /dev/null
# The three programs a freshly built kernel needs to boot, at 32 bits.
init32.c4r: c4cc32 $(INIT_SRCS)
	./c4cc32 -o $@ $(INIT_SRCS) > /dev/null
c4sh32.c4r: c4cc32 $(C4SH_SRCS)
	./c4cc32 -o $@ $(C4SH_SRCS) > /dev/null
c4ke.vfs32.c4r: c4cc32 $(VFS_SRCS)
	./c4cc32 -o $@ $(VFS_SRCS) > /dev/null

# The three C4KE programs the full climb needs after the kernel boots:
# tar unpacks C4IX's source into the RAM filesystem, b4ke drives the
# twelve compiles and the link, and the shell needs something to list.
# The two halves of "keep what you just built": one transient for
# C4DOS's RAM disk, one program for C4KE's ramfs, both driving c4bb's
# disk registers through include/c4bb.h. Board only, like dostar and
# dosload -- docs/c4bb-storage.md.
bbsave32.c4r: c4cc32 include/c4dos.h include/c4bb.h $(SRCS)/c4dos/bbsave.c
	./c4cc32 -o $@ include/c4dos.h include/c4bb.h $(SRCS)/c4dos/bbsave.c > /dev/null
install32.c4r: c4cc32 include/c4dos.h include/c4bb.h $(SRCS)/c4dos/install.c
	./c4cc32 -o $@ include/c4dos.h include/c4bb.h $(SRCS)/c4dos/install.c > /dev/null
# The same job one rung up, as a C4KE task rather than a C4DOS
# transient. It has its own name because the DOS one is on the same
# floppy and cannot run here -- it talks to a DOS that is gone.
kinstall32.c4r: c4cc32 $(U0) include/c4bb.h $(BIN_D)/install.c
	./c4cc32 -o $@ $(U0) include/c4bb.h $(BIN_D)/install.c > /dev/null
save32.c4r: c4cc32 $(U0) include/c4bb.h $(BIN_D)/save.c
	./c4cc32 -o $@ $(U0) include/c4bb.h $(BIN_D)/save.c > /dev/null

# reboot: no u0, no DOS API, no kernel -- the same image is a C4DOS
# transient, a C4KE task and a C4IX program, because all it does is
# write a device register.
reboot32.c4r: c4cc32 include/c4bb.h $(SRCS)/c4bb/tools/reboot.c
	./c4cc32 -o $@ include/c4bb.h $(SRCS)/c4bb/tools/reboot.c > /dev/null

b4ke32.c4r: c4cc32 $(U0) $(BIN_D)/b4ke.c
	./c4cc32 -o $@ $(U0) $(BIN_D)/b4ke.c > /dev/null
tar.c4r: $(C4CC) $(U0) $(BIN_D)/tar.c
	$(C4CC) -o $@ $(U0) $(BIN_D)/tar.c > /dev/null
tar32.c4r: c4cc32 $(U0) $(BIN_D)/tar.c
	./c4cc32 -o $@ $(U0) $(BIN_D)/tar.c > /dev/null
ls32.c4r: c4cc32 $(U0) $(BIN_D)/ls.c
	./c4cc32 -o $@ $(U0) $(BIN_D)/ls.c > /dev/null
ps32.c4r: c4cc32 $(U0) $(BIN_D)/ps.c
	./c4cc32 -o $@ $(U0) $(BIN_D)/ps.c > /dev/null

c4rlink32.c4r: c4cc32 $(C4R_C4CC_SRCS) $(SRCS)/c4ke/bin/c4rlink.c
	./c4cc32 -o $@ $(C4R_C4CC_SRCS) $(SRCS)/c4ke/bin/c4rlink.c > /dev/null
c4rlink32: $(SRCS)/c4ke/bin/c4rlink.c $(SRCS)/c4cc/asm-c4r.c
	gcc -m32 $(NATIVE_CC_OPTS) -Isrc/c4cc -o c4rlink32 $(SRCS)/c4ke/bin/c4rlink.c -lm
c4bb-32bit: c4cc32 c4m32 c4sp32 c4rlink32
# c4th32.c4r is copied by build-images.sh (the fused-opcode disk) but was
# not named here, so the target only worked when some earlier build had
# happened to leave that artifact lying around. It is gitignored, so a
# fresh tree -- or any tree where it has since been cleaned away -- got
# `cp: cannot stat 'c4th32.c4r'` and a failed test-c4bb. Name it and the
# target builds what it uses.
# ./cpp is a hard prerequisite now, not just for C4DOS: the shared
# disk's c4m.c4r goes through it (build-images.sh).
c4bb-images: c4bb-32bit $(C4LC_LISP) c4th32.c4r cpp
	bash src/c4bb/tests/build-images.sh
test-c4bb: c4bb-images $(C4M) $(TESTS_C4R)
	bash src/c4bb/tests/test-c4bb.sh

# Drives and media (docs/c4bb-storage.md). Separate from test-c4bb
# because it needs the C4DOS climb disk, which test-c4bb does not build.
test-c4bb-storage: c4dos32.c4r $(C4DOS_IX_DISK)
	bash src/c4bb/tests/test-storage.sh

# The firmware's four stages (M6): each does its own job and refuses
# the next one's, and the parts a stage has not got are missing from the
# image rather than skipped at run time.
test-c4bb-firmware: src/c4bb/images/climb
	bash src/c4bb/tests/test-firmware.sh

# The browser front-end, in a real browser on real hardware (M5, M6).
# It talks over CDP to the browser the user has open -- the same
# endpoint ~/git/Homeward's harnesses use, reached by the SSH tunnel
# that runs outward from that machine to here. `--local` launches a
# headless one instead.
#
# Skips and passes when there is no browser and no Playwright: a test
# that cannot run is not a test that failed, and `make test-c4bb` has
# neither. What it covers is what test-drives.mjs cannot -- the panel
# drawing, a firmware stage refusing to boot, and a medium the machine
# wrote being STILL THERE after a reload.
test-c4bb-web: src/c4bb/images/climb
	node src/c4bb/tests/test-web.mjs

src/c4bb/images/climb: | $(C4DOS_IX_DISK)
	bash src/c4bb/tests/build-images.sh

# The whole climb (M11 and M12): three power-ons, three systems, each
# booted from a medium the previous one wrote. M11's bar is a strict
# prefix of M12's, so they share one script rather than building the
# same kernel twice.
test-c4bb-climb: c4dos32.c4r $(C4DOS_IX_DISK)
	bash src/c4bb/tests/test-climb.sh

test-c4bb-install: test-c4bb-climb

# What each rung is allowed to need, and what it must need
# (docs/c4bb-storage.md M15). HOMEWARD's ladder is a ladder of opcodes:
# the player extends their own CPU to climb it, so an image that reaches
# past its rung is a milestone given away for free -- and an image that
# does NOT reach past the rung below is a milestone that was never there
# at all. `opscan -rungs` prints the table; it is defined in one place,
# in the tool.
OPSCAN := node src/c4bb/tools/opscan.mjs -q

# The images at each rung. Named here rather than inline so that the
# ceiling test and the floor test below cannot drift apart.
RUNG_BASE  := src/c4bb/fw/fw.c4r src/c4bb/fw/fw-hello.c4r \
              src/c4bb/fw/fw-ram.c4r src/c4bb/fw/fw-drives.c4r \
              dostar32.c4r dosload32.c4r reboot32.c4r \
              bbsave32.c4r install32.c4r cpp32.c4r c4-dos32.c4r c4m-dos32.c4r \
              rps-dos32.c4r
RUNG_DOS   := c4dos32.c4r mandel-dos32.c4r raycast-dos32.c4r
RUNG_C4M   := src/c4bb/images/c4ke32.c4r src/c4bb/images/c4ix32.c4r \
              c4cc32.c4r c4rlink32.c4r init32.c4r c4sh32.c4r ls32.c4r \
              ps32.c4r tar32.c4r b4ke32.c4r save32.c4r c4ke.vfs32.c4r \
              src/c4bb/images/disk/c4ix-sh.c4r src/c4bb/images/disk/c4ix-ps.c4r
RUNG_FUSED := c4sc32-fused.c4r src/c4bb/images/factorial-fused.c4r

test-c4bb-rungs: src/c4bb/fw/fw.c4r $(RUNG_BASE) $(RUNG_DOS) $(RUNG_C4M) $(RUNG_FUSED) \
                 src/c4bb/images/c4ke32.c4r
	@# 1. The top rung is exactly the machine. Read out of
	@#    hw/microcode.uc, so an opcode gained or lost there without a
	@#    rung to put it in says so here rather than never.
	$(OPSCAN) -machine
	@# 2. Ceilings. Nothing may reach above its own rung.
	$(OPSCAN) -rung base  $(RUNG_BASE)
	$(OPSCAN) -rung dos   $(RUNG_DOS)
	$(OPSCAN) -rung c4m   $(RUNG_C4M)
	$(OPSCAN) -rung fused $(RUNG_FUSED)
	@# 3. Floors, and this is the half that makes the ladder real. Each
	@#    rung's entry image must NOT fit the rung below -- otherwise
	@#    "the player must extend the CPU before this" is a sentence in
	@#    a document and nothing else.
	$(OPSCAN) -needs base c4dos32.c4r
	$(OPSCAN) -needs dos  src/c4bb/images/c4ke32.c4r c4cc32.c4r
	$(OPSCAN) -needs c4m  $(RUNG_FUSED)
	@echo "test-c4bb-rungs: OK -- four rungs, each image inside its own"
	@echo "                       and above the one below"

# The old name for the above, which docs and muscle memory still use.
test-c4bb-baseops: test-c4bb-rungs

# C4OR1K: OR1000/OpenRISC emulator ported from jor1k, compiled by
# c4lc, run under c4m (docs/c4or1k-design.md).
#
# M0 (throughput sanity check) was a single main.c; from M1 the
# decoder is real and lives in its own module, linked with c4rlink
# the same way src/c4mp does it.

# Per-module c4lc compiles are independent, and each takes a few
# seconds, so run them in parallel. C4LC_JOBS defaults to nproc (this
# box has 6 cores; ~11 c4or1k modules => roughly 2x wall-clock). Any
# module's non-zero exit propagates through xargs (exit 123), failing
# the recipe. $(1)=c4lc flags, $(2)=modules, $(3)=include dir,
# $(4)=object-name prefix. The `{}` is xargs's per-module substitution.
C4LC_JOBS ?= $(shell nproc 2>/dev/null || echo 4)
define c4lc_compile_par
	printf '%s\n' $(2) | xargs -P $(C4LC_JOBS) -I{} sh -c \
		'$(C4SPLC) src/c4sp/lisp/c4lc.lisp $(1) -c -I $(3) $(3)/{}.c $(4){}.c4o > /dev/null' \
		|| { echo "c4lc: a parallel module compile failed"; exit 1; }
endef
C4OR1K_SRC  := src/c4or1k
C4OR1K_MODS := mem mmio uart console bootfs virtio virtio9p eth net fpu cpu boot main

# Parallel per-module c4lc compile. The modules are independent .c ->
# .c4o objects, and a single c4lc invocation is slow, so run several
# at once via xargs -P. C4LC_JOBS defaults to nproc.
# $(1)=extra c4lc flags  $(2)=module list  $(3)=-I dir  $(4)=obj prefix
C4LC_JOBS ?= $(shell nproc 2>/dev/null || echo 4)
define c4lc_compile_par
printf '%s\n' $(2) | xargs -P $(C4LC_JOBS) -I@@ sh -c '$(C4SPLC) src/c4sp/lisp/c4lc.lisp $(1) -c -I $(3) $(3)/@@.c $(4)@@.c4o >/dev/null'
endef
# M13: the JIT build carries one extra module and compiles everything
# with -D C4OR1K_JIT=1, which is what actually enables the driver-loop
# hooks in cpu.c/mem.c/main.c -- without the define those compile to
# exactly the M12 code, so the default and -mcisc images pay nothing.
C4OR1K_JIT_MODS := mem mmio uart console bootfs virtio virtio9p eth net fpu jit cpu boot main
C4OR1K_HDRS := $(C4OR1K_SRC)/cpu.h $(C4OR1K_SRC)/mem.h $(C4OR1K_SRC)/jit.h $(C4OR1K_SRC)/fpu.h
# -O (M7): ~4% faster on a real boot workload, byte-for-byte identical
# output on the full M1-M4 regression suite and a real boot -- a small
# but real, zero-cost win worth keeping on by default given how many
# guest instructions a full boot to a shell needs (see M6).
c4or1k.c4r: c4sp $(C4RLINK) $(C4LC_LISP) $(C4OR1K_HDRS) $(patsubst %,$(C4OR1K_SRC)/%.c,$(C4OR1K_MODS))
	$(call c4lc_compile_par,-O,$(C4OR1K_MODS),$(C4OR1K_SRC),.c4or1k_)
	$(C4RLINK) $(patsubst %,.c4or1k_%.c4o,$(C4OR1K_MODS)) -o c4or1k.c4r
	rm -f .c4or1k_*.c4o
# M12: -mcisc build, emitting c4mp's LXI/SXI fused array-element
# opcodes wherever c4lc-gen.lisp finds a plain var[idx] with an
# 8-byte-element (int/pointer) array -- cpu.c's r[]/group0[]/
# group1[]/group2[] all qualify. c4m cannot run this image (LXI/SXI
# trap as illegal opcodes there); only c4mp can, see c4or1k-boot-cisc
# below and docs/c4or1k-design.md's M12 section.
c4or1k-cisc.c4r: c4sp $(C4RLINK) $(C4LC_LISP) $(C4OR1K_HDRS) $(patsubst %,$(C4OR1K_SRC)/%.c,$(C4OR1K_MODS))
	$(call c4lc_compile_par,-mcisc -O,$(C4OR1K_MODS),$(C4OR1K_SRC),.c4or1k_cisc_)
	$(C4RLINK) $(patsubst %,.c4or1k_cisc_%.c4o,$(C4OR1K_MODS)) -o c4or1k-cisc.c4r
	rm -f .c4or1k_cisc_*.c4o
# M13: the JIT-enabled build (see C4OR1K_JIT_MODS above and jit.h's
# status note: correct and fully verified, but off by default and
# net-slower at current coverage -- run with `-jit` as the last
# argument to enable translation at runtime).
c4or1k-jit.c4r: c4sp $(C4RLINK) $(C4LC_LISP) $(C4OR1K_HDRS) $(C4OR1K_SRC)/jit.h $(patsubst %,$(C4OR1K_SRC)/%.c,$(C4OR1K_JIT_MODS))
	$(call c4lc_compile_par,-O -D C4OR1K_JIT=1,$(C4OR1K_JIT_MODS),$(C4OR1K_SRC),.c4or1k_jit_)
	$(C4RLINK) $(patsubst %,.c4or1k_jit_%.c4o,$(C4OR1K_JIT_MODS)) -o c4or1k-jit.c4r
	rm -f .c4or1k_jit_*.c4o
# M1's cross-checked test program (tests/m1_test.s -> .bin via
# tools/asm.py) run through the real decoder.
src/c4or1k/tests/m1_test.bin: src/c4or1k/tests/m1_test.s src/c4or1k/tools/asm.py
	python3 src/c4or1k/tools/asm.py src/c4or1k/tests/m1_test.s bin > src/c4or1k/tests/m1_test.bin
c4or1k-m1: c4m c4or1k.c4r src/c4or1k/tests/m1_test.bin
	./c4m load-c4r.c -- c4or1k.c4r src/c4or1k/tests/m1_test.bin
# Cross-checks cpu.c against jor1k's own safecpu.js (tools/or1k-oracle.js)
# on the same test program: every register, SR_F/SR_CY/SR_OV, and the
# RAM window every test result is written into must match exactly.
# (No process substitution: plain sh, not bash, runs make recipes here.)
c4or1k-m1-check: c4m c4or1k.c4r c4or1k-jit.c4r c4or1k-native src/c4or1k/tests/m1_test.bin
	./c4m load-c4r.c -- c4or1k.c4r src/c4or1k/tests/m1_test.bin | grep -v '^c4or1k:' | grep -v 'guest instructions/sec' > .c4or1k_check_c.txt
	node src/c4or1k/tools/or1k-oracle.js src/c4or1k/tests/m1_test.bin | grep -v '^c4or1k-oracle:' > .c4or1k_check_js.txt
	diff .c4or1k_check_c.txt .c4or1k_check_js.txt && echo "c4or1k-m1-check: OK (bit-for-bit match against jor1k)"
	./c4m load-c4r.c -- c4or1k-jit.c4r src/c4or1k/tests/m1_test.bin -jit | grep -v '^c4or1k:' | grep -v 'guest instructions/sec' > .c4or1k_check_c.txt
	diff .c4or1k_check_c.txt .c4or1k_check_js.txt && echo "c4or1k-m1-check: OK (bit-for-bit match against jor1k, -jit)"
	./c4or1k-native src/c4or1k/tests/m1_test.bin | grep -v '^c4or1k:' | grep -v 'guest instructions/sec' > .c4or1k_check_c.txt
	diff .c4or1k_check_c.txt .c4or1k_check_js.txt && echo "c4or1k-m1-check: OK (bit-for-bit match against jor1k, native)"
	rm -f .c4or1k_check_c.txt .c4or1k_check_js.txt
# M2's cross-checked test program: SPRs, SR flags, EXCEPT_SYSCALL/
# EXCEPT_TRAP/EXCEPT_DTLBMISS delivery, l.rfe.
src/c4or1k/tests/m2_test.bin: src/c4or1k/tests/m2_test.s src/c4or1k/tools/asm.py
	python3 src/c4or1k/tools/asm.py src/c4or1k/tests/m2_test.s bin > src/c4or1k/tests/m2_test.bin
c4or1k-m2: c4m c4or1k.c4r src/c4or1k/tests/m2_test.bin
	./c4m load-c4r.c -- c4or1k.c4r src/c4or1k/tests/m2_test.bin
c4or1k-m2-check: c4m c4or1k.c4r c4or1k-jit.c4r c4or1k-native src/c4or1k/tests/m2_test.bin
	./c4m load-c4r.c -- c4or1k.c4r src/c4or1k/tests/m2_test.bin | grep -v '^c4or1k:' | grep -v 'guest instructions/sec' > .c4or1k_check_c.txt
	node src/c4or1k/tools/or1k-oracle.js src/c4or1k/tests/m2_test.bin | grep -v '^c4or1k-oracle:' > .c4or1k_check_js.txt
	diff .c4or1k_check_c.txt .c4or1k_check_js.txt && echo "c4or1k-m2-check: OK (bit-for-bit match against jor1k)"
	./c4m load-c4r.c -- c4or1k-jit.c4r src/c4or1k/tests/m2_test.bin -jit | grep -v '^c4or1k:' | grep -v 'guest instructions/sec' > .c4or1k_check_c.txt
	diff .c4or1k_check_c.txt .c4or1k_check_js.txt && echo "c4or1k-m2-check: OK (bit-for-bit match against jor1k, -jit)"
	./c4or1k-native src/c4or1k/tests/m2_test.bin | grep -v '^c4or1k:' | grep -v 'guest instructions/sec' > .c4or1k_check_c.txt
	diff .c4or1k_check_c.txt .c4or1k_check_js.txt && echo "c4or1k-m2-check: OK (bit-for-bit match against jor1k, native)"
	rm -f .c4or1k_check_c.txt .c4or1k_check_js.txt
# M3: UART + console I/O. tests/m3_echo.s polls LSR and echoes every
# received byte back out through TXBUF, halting on Ctrl-D (0x04).
# c4or1k-m3 is the interactive form (real terminal, via
# run-c4or1k.sh's raw-mode wrapper); c4or1k-m3-check pipes a known
# string through non-interactively and diffs the echoed bytes.
src/c4or1k/tests/m3_echo.bin: src/c4or1k/tests/m3_echo.s src/c4or1k/tools/asm.py
	python3 src/c4or1k/tools/asm.py src/c4or1k/tests/m3_echo.s bin > src/c4or1k/tests/m3_echo.bin
c4or1k-m3: c4m c4or1k.c4r src/c4or1k/tests/m3_echo.bin
	./src/c4or1k/run-c4or1k.sh c4or1k.c4r src/c4or1k/tests/m3_echo.bin -r
c4or1k-m3-check: c4m c4or1k.c4r src/c4or1k/tests/m3_echo.bin
	printf 'hello, c4or1k\004' > .c4or1k_check_in.txt
	./src/c4or1k/run-c4or1k.sh c4or1k.c4r src/c4or1k/tests/m3_echo.bin -r < .c4or1k_check_in.txt > .c4or1k_check_out.txt
	diff .c4or1k_check_in.txt .c4or1k_check_out.txt && echo "c4or1k-m3-check: OK (echoed byte-for-byte, including Ctrl-D)"
	rm -f .c4or1k_check_in.txt .c4or1k_check_out.txt
# Same echo test, but interrupt-driven (EXCEPT_INT via PICMR/PICSR/
# SR_IEE) instead of polled -- exercises the whole M2+M3 IRQ pipeline.
src/c4or1k/tests/m3_echo_int.bin: src/c4or1k/tests/m3_echo_int.s src/c4or1k/tools/asm.py
	python3 src/c4or1k/tools/asm.py src/c4or1k/tests/m3_echo_int.s bin > src/c4or1k/tests/m3_echo_int.bin
c4or1k-m3-int-check: c4m c4or1k.c4r src/c4or1k/tests/m3_echo_int.bin
	printf 'hello, c4or1k\004' > .c4or1k_check_in.txt
	./src/c4or1k/run-c4or1k.sh c4or1k.c4r src/c4or1k/tests/m3_echo_int.bin -r < .c4or1k_check_in.txt > .c4or1k_check_out.txt
	diff .c4or1k_check_in.txt .c4or1k_check_out.txt && echo "c4or1k-m3-int-check: OK (interrupt-driven echo, byte-for-byte)"
	rm -f .c4or1k_check_in.txt .c4or1k_check_out.txt
# M4: boot loader. VMLINUX is jorconsole's already-decompressed image
# (see boot.h); -b MAXSTEPS bounds a boot attempt by instruction
# count, since there's no panic detection yet to stop on its own. The
# default here only reaches the pre-M5 VFS panic quickly, as a fast
# sanity check -- reaching the real interactive shell M6 added needs
# N in the hundreds of millions to low billions (mostly udhcpc's own
# DHCP retry/backoff timing) and `stdbuf -oL ./c4m ...` to see output
# live rather than losing it to stdio's full-buffering on a crash or
# early -b cutoff. Try e.g. `stdbuf -oL ./c4m load-c4r.c -- c4or1k.c4r
# $(VMLINUX) -b 700000000 <bootfs.idx> <bootfs.blob>` directly.
VMLINUX := ../jorconsole/jor1k-sysroot/or1k/vmlinux.bin
N := 2000000
# M5: 9p root filesystem, basefs.json only (docs/c4or1k-design.md).
# tools/mkbootfs.js flattens basefs.json + its real file content
# (decompressing bin/busybox.bz2 via the host's bunzip2) into
# bootfs.idx/bootfs.blob offline -- bootfs.c has no JSON parser and no
# network, see that tool's header comment.
BASEFS_JSON := ../jorconsole/jor1k-sysroot/or1k/basefs.json
BASEFS_SRC  := ../jorconsole/jor1k/sys/or1k/basefs
src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob: src/c4or1k/tools/mkbootfs.js $(BASEFS_JSON)
	mkdir -p src/c4or1k/images
	node src/c4or1k/tools/mkbootfs.js src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob $(BASEFS_JSON) $(BASEFS_SRC)
# The EXTENDED filesystem: basefs.json overlaid with jor1k's full
# fs.json (~7000 inodes, ~127MB of bz2-compressed backing -- the real
# Debian/OpenRISC userland: bash, perl, X libs, man pages, ...). Built
# by the same tool from BOTH manifests merged (mkbootfs.js's header
# explains the fs.json overlay + dir-merge). NATIVE-ONLY for now: the
# resident blob is far too large to be practical under c4m/c4mp (the
# user's call -- prove it native first, backport the loading strategy
# later if it's worth it). bootfs.c loads it exactly like the small one.
EXTFS_JSON := ../jorconsole/jor1k-sysroot/fs.json
EXTFS_SRC  := ../jorconsole/jor1k-sysroot/fs
src/c4or1k/images/bootfs-ext.idx src/c4or1k/images/bootfs-ext.blob: src/c4or1k/tools/mkbootfs.js $(BASEFS_JSON) $(EXTFS_JSON)
	mkdir -p src/c4or1k/images
	node src/c4or1k/tools/mkbootfs.js src/c4or1k/images/bootfs-ext.idx src/c4or1k/images/bootfs-ext.blob \
		$(BASEFS_JSON) $(BASEFS_SRC) $(EXTFS_JSON) $(EXTFS_SRC)
c4or1k-boot: c4m c4or1k.c4r src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
	./c4m load-c4r.c -- c4or1k.c4r $(VMLINUX) -b $(N) src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
# M11: c4mp (src/c4mp) is opcode-for-opcode compatible with c4m -- a
# guest .c4r image cannot tell the two apart -- and runs any .c4r
# directly (no load-c4r.c wrapper). Verified byte-for-byte identical
# boot output against c4m, ~18-20% faster wall-clock with zero source
# changes to c4or1k (docs/c4or1k-design.md's M11 section). c4m itself
# is intentionally NOT modified to get this -- see that section for
# why (plain c4 must still be able to parse c4m.c).
c4or1k-boot-mp: c4mp c4or1k.c4r src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
	./c4mp c4or1k.c4r $(VMLINUX) -b $(N) src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
# M12: the -mcisc build (LXI/SXI fused array-element opcodes) --
# requires c4mp, c4m cannot run it at all (see c4or1k-cisc.c4r above).
c4or1k-boot-cisc: c4mp c4or1k-cisc.c4r src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
	./c4mp c4or1k-cisc.c4r $(VMLINUX) -b $(N) src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
# M13 v2: the JIT build -- the fastest HOSTED configuration (~28% over
# M12 on the standard 60M-instruction benchmark, byte-identical
# output; see docs/c4or1k-design.md's M13 section). Translation is on
# by default in this image; pass -nojit (last) to disable at runtime.
c4or1k-boot-jit: c4mp c4or1k-jit.c4r src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
	./c4mp c4or1k-jit.c4r $(VMLINUX) -b $(N) src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
# M13: the NATIVE build -- the same c4or1k sources compiled directly
# by gcc (c4mp-style dual-build; src/c4or1k/native.h maps the c4lc
# dialect onto the host, exactly c4.c's own `#define int long` trick).
# No c4m, no c4mp, no JIT: this measures the emulator alone, with the
# interpretation tax at zero. Same argv contract as the hosted builds.
c4or1k-native: $(patsubst %,$(C4OR1K_SRC)/%.c,$(C4OR1K_MODS)) $(C4OR1K_HDRS) $(C4OR1K_SRC)/native.h
	gcc -O2 -w -include $(C4OR1K_SRC)/native.h \
		$(patsubst %,$(C4OR1K_SRC)/%.c,$(C4OR1K_MODS)) -o c4or1k-native
c4or1k-boot-native: c4or1k-native src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
	./c4or1k-native $(VMLINUX) -b $(N) src/c4or1k/images/bootfs.idx src/c4or1k/images/bootfs.blob
# The extended-filesystem boot -- NATIVE ONLY (see the bootfs-ext rule).
# The full userland reaches an interactive shell with real bash, perl,
# man, vi, etc. instead of just busybox. Same emulator, larger backing.
c4or1k-boot-native-ext: c4or1k-native src/c4or1k/images/bootfs-ext.idx src/c4or1k/images/bootfs-ext.blob
	./c4or1k-native $(VMLINUX) -b $(N) src/c4or1k/images/bootfs-ext.idx src/c4or1k/images/bootfs-ext.blob
test-oisc4-nested: $(OISC4) $(C4) $(C4M) oisc4-lc.c4r $(TESTS)/hello.c4r
	$(C4M) load-c4r.c -- oisc4-lc.c4r -m 32 $(TESTS)/hello.c4r | grep -q yello
	$(C4) c4l.c oisc4-lc.c4r -m 32 $(TESTS)/hello.c4r | grep -q yello
	$(OISC4) -m 192 oisc4-lc.c4r -m 32 $(TESTS)/hello.c4r | grep -q yello
	@echo "test-oisc4-nested: OK"

# c4mp (docs/c4mp-design.md): the multiprocessor VM. Unlike c4m it
# carries no compiler -- it loads .c4r images -- and unlike c4m it is
# compiled by c4lc, so it may use structs, for, -> and a preprocessor.
# Two builds from one source: a native executable, and an image that
# c4m can host. The second is a backwards-compatibility proof, since
# c4m has no multiprocessing to offer.
C4MP_SRC  := src/c4mp
C4MP_MODS := vm smp loader main
C4MP_HDRS := $(C4MP_SRC)/c4mp.h $(INCLUDE)/c4.h $(INCLUDE)/c4m_float.h $(INCLUDE)/c4m_util.h
c4mp: $(patsubst %,$(C4MP_SRC)/%.c,$(C4MP_MODS)) $(C4MP_HDRS) c4m_float.c
	$(NATIVE_CC) $(NATIVE_CC_OPTS) -I $(C4MP_SRC) \
		$(patsubst %,$(C4MP_SRC)/%.c,$(C4MP_MODS)) c4m_float.c -o c4mp -lm
# No gcc here, the same way C4IX does it: c4lc preprocesses the modules
# itself (L9). -D __c4cc__=1 is what the gcc -E path defines, and it is
# how c4mp.h decides whether restrict and the host headers exist.
c4mp.c4r: c4sp $(C4RLINK) $(C4LC_LISP) $(C4MP_SRC)/c4mp.h $(patsubst %,$(C4MP_SRC)/%.c,$(C4MP_MODS))
	for m in $(C4MP_MODS); do \
		$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O -c -I $(C4MP_SRC) -D __c4cc__=1 \
			$(C4MP_SRC)/$$m.c .c4mp_$$m.c4o > /dev/null || exit 1; \
	done
	$(C4RLINK) $(patsubst %,.c4mp_%.c4o,$(C4MP_MODS)) -o c4mp.c4r
	rm -f .c4mp_*.c4o
# The stage-2 guest: starts every processor the machine offers and
# proves they interleave. Compiled by c4lc because it calls the
# processor opcodes, which c4cc does not know.
c4mp-smp0.c4r: c4sp $(C4LC_LISP) $(C4MP_SRC)/guest/smp0.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O $(C4MP_SRC)/guest/smp0.c c4mp-smp0.c4r > /dev/null
# smp1 exercises every stage-3 opcode; deadlock exists to wedge the
# machine on purpose and be diagnosed for it.
c4mp-smp1.c4r: c4sp $(C4LC_LISP) $(C4MP_SRC)/guest/smp1.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O $(C4MP_SRC)/guest/smp1.c c4mp-smp1.c4r > /dev/null
c4mp-deadlock.c4r: c4sp $(C4LC_LISP) $(C4MP_SRC)/guest/deadlock.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O $(C4MP_SRC)/guest/deadlock.c c4mp-deadlock.c4r > /dev/null
# Not in the default suite: the native sweep runs every test image
# through two VMs and the hosted checks run them through three.
test-c4mp: c4m c4mp c4mp.c4r c4mp-smp0.c4r c4mp-smp1.c4r c4mp-deadlock.c4r $(TESTS_C4R)
	bash $(C4MP_SRC)/test-c4mp.sh

# C4IX (docs/c4ix-design.md): the c4lc-compiled OS. Each module is
# preprocessed, compiled to a .c4o object with full optimization, and
# the kernel image is linked by c4rlink.
C4IX_SRC  := src/c4ix
C4IX_MODS := boot console va host sl4b task sched vfs sys c4ke loader init
c4ix.c4r: c4sp $(C4RLINK) $(C4LC_LISP) $(C4IX_SRC)/c4ix.h $(patsubst %,$(C4IX_SRC)/%.c,$(C4IX_MODS))
	@# No gcc here: c4lc preprocesses the modules itself (L9). Each
	@# object is byte-identical to the gcc -E path, pinned by test-c4lc.
	@# The twelve modules are independent .c -> .c4o compiles, so run
	@# them in parallel exactly as the c4or1k rules do. c4lc_compile_par
	@# propagates any module's failure through xargs.
	$(call c4lc_compile_par,-O,$(C4IX_MODS),$(C4IX_SRC),.c4ix_)
	$(C4RLINK) $(patsubst %,.c4ix_%.c4o,$(C4IX_MODS)) -o c4ix.c4r
	rm -f .c4ix_*.pp.c .c4ix_*.c4o

# the spawn-test userland program: raw opcodes, no library. Under
# protected mode its printf traps and the kernel emulates it onto the
# fd layer -- redirection for programs that never heard of C4IX.
c4ix-hello.c4r: c4sp $(C4LC_LISP) $(C4IX_SRC)/user/hello.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O $(C4IX_SRC)/user/hello.c c4ix-hello.c4r > /dev/null

# libc4ix, the userland C library, as a c4rlink archive
libc4ix.c4l: c4sp $(C4RLINK) $(C4LC_LISP) $(C4IX_SRC)/lib/libc4ix.c $(C4IX_SRC)/include/c4ix_user.h
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O -c -I $(C4IX_SRC)/include $(C4IX_SRC)/lib/libc4ix.c .c4ix_lib.c4o > /dev/null
	$(C4RLINK) -r .c4ix_lib.c4o -o libc4ix.c4l
	rm -f .c4ix_lib.pp.c .c4ix_lib.c4o

# userland programs built against the library: all IO via syscalls
c4ix-%.c4r: c4sp $(C4RLINK) $(C4LC_LISP) libc4ix.c4l $(C4IX_SRC)/user/%.c
	$(C4SPLC) src/c4sp/lisp/c4lc.lisp -O -c -I $(C4IX_SRC)/include $(C4IX_SRC)/user/$*.c .c4ix_u.c4o > /dev/null
	$(C4RLINK) .c4ix_u.c4o libc4ix.c4l -o $@
	rm -f .c4ix_u.pp.c .c4ix_u.c4o

# X3 boot pins: the linked kernel boots natively on c4m (preemptive,
# user tasks behind protected mode) and degraded on plain c4 through
# the c4l loader (cooperative, no hardware boundary).
#
# The ping/pong lines in this pin record a SCHEDULING interleaving, and
# it moves when the kernel's cost does: teaching c4r_load to look in the
# RAM filesystem (docs/c4th-design.md, B6) swapped "ping 2" and
# "pong 2" and changed nothing else. That is a measurement sitting in a
# behaviour pin -- deterministic, so it is not flaky, but it means any
# change to the kernel's timing shows up here as a diff to read rather
# than a regression to fix. Output is exact
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
# The info word is the HOST's capability bits, not C4IX behaviour:
# native c4m reports 0xf2, c4m interpreted by plain c4 reports 0x83
# (no high-resolution timer, signals or floating point down there).
# The line below it says in words what actually matters.
C4IX_MASK := sed -E -e 's/[0-9]+ cycles/N cycles/g' -e 's/info 0x[0-9a-f]+/info 0xHH/'
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
# The C4KE compatibility layer: C4KE's own binaries, UNMODIFIED,
# running as C4IX tasks. These are the same files C4KE's suite runs --
# if one ever needs a rebuild to pass, the compatibility layer is
# wrong, and that is the strongest check available here.
#
# Numbers are masked because they are measurements: cycle counts,
# timings and percentages all move between runs. A digit run survives
# only when a letter precedes it, so c4ix, c4m, sl4b and ps.c4r stay
# readable while "27.598 M" becomes "N.N M". Whitespace is squeezed
# too, because ps sizes its columns from the data it is given; column
# layout is pinned exactly by test-c4ix-fmt instead, which is the test
# that exists to catch formatter regressions.
#
# spin and benchtop never exit by design, so they are smoke-tested
# under a timeout rather than pinned.
C4KE_MASK := sed -E -e 's/^[0-9]+/N/' -e 's/([^A-Za-z])[0-9]+/\1N/g' \
                    -e 's/[[:space:]]+/ /g' -e 's/ $$//'

test-c4ix-c4ke: c4m c4ix.c4r pre
	$(C4M) load-c4r.c -- c4ix.c4r ps.c4r | $(C4KE_MASK) | cmp - $(C4IX_SRC)/tests/ck-ps.txt
	$(C4M) load-c4r.c -- c4ix.c4r top.c4r -b -n 2 | $(C4KE_MASK) | cmp - $(C4IX_SRC)/tests/ck-top.txt
	$(C4M) load-c4r.c -- c4ix.c4r bench.c4r -q | $(C4KE_MASK) | cmp - $(C4IX_SRC)/tests/ck-bench.txt
	$(C4M) load-c4r.c -- c4ix.c4r innerbench.c4r -B -n 2 -q -T | $(C4KE_MASK) | cmp - $(C4IX_SRC)/tests/ck-innerbench.txt
	@# Neither of these ever exits on its own.
	timeout 20 stdbuf -o0 $(C4M) load-c4r.c -- c4ix.c4r spin.c4r 2>&1 | grep -q "running forever"
	timeout 90 stdbuf -o0 $(C4M) load-c4r.c -- c4ix.c4r benchtop.c4r 2>&1 | grep -q "Benchmark complete"
	@# What the mask deliberately destroys: the reaper keeps the zombie
	@# column at zero even though innerbench never waits on its children.
	$(C4M) load-c4r.c -- c4ix.c4r innerbench.c4r -B -n 3 -q | grep -q "0 zombie"
	@echo "test-c4ix-c4ke: OK"

# innerbench's DEFAULT mode: each "benchmark" is a whole C4KE compiled
# from source inside a nested c4m, running as a C4IX task. It works --
# a nested kernel's ITH is absorbed by the nested interpreter and never
# reaches the host -- but it takes minutes, so it stays out of the
# pinned suite for the same reason test-c4ix-c4 does.
test-c4ix-c4ke-nested: c4m c4ix.c4r c4m.c4r pre
	$(C4M) load-c4r.c -- c4ix.c4r innerbench.c4r -n 1 -T | grep -q "benchmark processes finished"
	@echo "test-c4ix-c4ke-nested: OK"

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
# raycast: the renderer is deterministic by construction -- a
# word-size-independent PRNG, demo mode consumes no input, and -n
# bounds the run -- so a fixed seed gives byte-identical frames. The
# 32-vs-64-bit leg is the one that matters: a Q12 intermediate that
# wraps on c4bb's 32-bit words and not on native 64-bit ones shows up
# here as a frame diff instead of as a subtly wrong picture nobody
# notices. f/s is masked because it is wall-clock; kc is not masked,
# because the instruction count per frame IS deterministic.
RAYCAST_ARGS := 21x21 -s 7 -d -n 20 -g 80x22
RAYCAST_MASK := sed -E 's,f/s [ 0-9]{5},f/s XXXXX,g'

test-raycast: c4sp c4sp32 $(C4M) c4m32 $(TESTS)/raycast.c4r
	./c4m load-c4r.c -- $(TESTS)/raycast.c4r $(RAYCAST_ARGS) \
		| $(RAYCAST_MASK) | cmp - $(TESTS)/raycast.golden.txt
	./c4sp32 src/c4sp/lisp/c4lc.lisp -O -conforming -D RC_KE=1 \
		$(TESTS)/raycast.c .raycast32.c4r > /dev/null
	./c4m32 load-c4r.c -- .raycast32.c4r $(RAYCAST_ARGS) \
		| $(RAYCAST_MASK) | cmp - $(TESTS)/raycast.golden.txt
	./c4m load-c4r.c -- $(TESTS)/raycast.c4r 15x15 -s 3 -M | cmp - $(TESTS)/raycast.maze.txt
	./c4m load-c4r.c -- $(TESTS)/raycast.c4r 15x15 -s 3 -d -n 3000 -g 80x22 \
		| grep -qE "seen +97"
	@# The interactive path, which demo mode never touches. -K forces
	@# the keys to come from stdin rather than /dev/tty, so this behaves
	@# the same with or without a controlling terminal. Six keys plus
	@# the frame that processes 'q' is seven frames of 14 rows: it fails
	@# if the key queue is not drained after end-of-input retires the
	@# descriptor, which is how the batched case used to lose keys.
	test $$(printf 'ddwwwwq' | ./c4m load-c4r.c -- $(TESTS)/raycast.c4r \
		15x15 -s 3 -i -K -g 40x14 2>&1 | grep -c '48;5;') -eq 98
	@rm -f .raycast32.c4r
	@echo "test-raycast: OK"

test-c4lc: c4sp c4sp.c4r c4m $(C4CC) $(C4RLINK) $(C4KE_C4R) $(TESTS)/test_ramcc.c4r
	./c4sp src/c4sp/lisp/c4lc-tokens.lisp src/tests/c4lc_lex_sample.c | cmp - src/c4sp/tests/expected/c4lc-tokens.txt
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/c4lc-tokens.lisp src/tests/c4lc_lex_sample.c | cmp - src/c4sp/tests/expected/c4lc-tokens.txt
	./c4sp src/c4sp/lisp/c4lc-tokens.lisp -count src/c4cc/c4cc.c | grep -q "^tokens [0-9]"
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
			./c4sp src/c4sp/lisp/c4lc-ast.lisp -check .c4lc_pp.c | grep -q "^parse ok" || exit 1;; \
		*) \
			./c4sp src/c4sp/lisp/c4lc-ast.lisp -check $$f | grep -q "^parse ok" || exit 1;; \
		esac; \
	done
	# L1: the exact self-compile unit c4cc consumes (raw concatenation,
	# no cpp -- c4cc skips '#' lines), and preprocessed c4sp.c
	cat $(U0) load-c4r.c $(SRCS)/c4cc/c4cc.c $(SRCS)/c4cc/asm-c4r.c > .c4lc_cat.c
	./c4sp src/c4sp/lisp/c4lc-ast.lisp -check .c4lc_cat.c | grep -q "^parse ok"
	$(PREPROC) src/c4sp/c4sp.c > .c4lc_pp.c
	./c4sp src/c4sp/lisp/c4lc-ast.lisp -check .c4lc_pp.c | grep -q "^parse ok"
	rm -f .c4lc_cat.c .c4lc_pp.c
	# L2: code generation. The L2 sample compiles under both c4cc and
	# c4lc and the two binaries must behave identically under c4m.
	# for/continue (which c4cc cannot compile: its For branch is dead
	# code) check against committed gcc-generated output. c4lc output
	# must also roundtrip through c4r.lisp byte-identically and stay
	# correct after the c4opt passes.
	$(C4CC) -o .c4lc_a.c4r src/tests/c4lc_l2.c
	./c4sp src/c4sp/lisp/c4lc.lisp src/tests/c4lc_l2.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_a.c4r > .c4lc_out_a
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - .c4lc_out_a
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp .c4lc_b.c4r | grep -q "roundtrip identical"
	./c4sp src/c4sp/lisp/c4opt-run.lisp .c4lc_b.c4r .c4lc_bo.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_bo.c4r | cmp - .c4lc_out_a
	./c4sp src/c4sp/lisp/c4lc.lisp src/tests/c4lc_for.c .c4lc_f.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_f.c4r | cmp - src/c4sp/tests/expected/c4lc-for.txt
	rm -f .c4lc_a.c4r .c4lc_b.c4r .c4lc_bo.c4r .c4lc_f.c4r .c4lc_out_a
	# L3: full subset. Every deterministic raw test c4cc compiles must
	# behave byte-identically when compiled by c4lc; tests that print
	# runtime addresses compare with pointers masked; the variadic
	# tests go through the preprocessor (stdarg.h + __c4cc_make_va).
	for t in $(C4LC_DIFF); do \
		$(C4CC) -o .c4lc_a.c4r src/tests/$$t.c > /dev/null 2>&1 || exit 1; \
		./c4m load-c4r.c -- .c4lc_a.c4r > .c4lc_out_a 2>&1; \
		./c4sp src/c4sp/lisp/c4lc.lisp src/tests/$$t.c .c4lc_b.c4r > /dev/null || exit 1; \
		./c4m load-c4r.c -- .c4lc_b.c4r 2>&1 | cmp - .c4lc_out_a || exit 1; \
	done
	for t in $(C4LC_DIFF_MASKED); do \
		$(C4CC) -o .c4lc_a.c4r src/tests/$$t.c > /dev/null 2>&1 || exit 1; \
		./c4m load-c4r.c -- .c4lc_a.c4r 2>&1 | sed -E 's/0x[0-9a-f]+/ADDR/g' > .c4lc_out_a; \
		./c4sp src/c4sp/lisp/c4lc.lisp src/tests/$$t.c .c4lc_b.c4r > /dev/null || exit 1; \
		./c4m load-c4r.c -- .c4lc_b.c4r 2>&1 | sed -E 's/0x[0-9a-f]+/ADDR/g' | cmp - .c4lc_out_a || exit 1; \
	done
	for t in $(C4LC_DIFF_PP); do \
		$(PREPROC) src/tests/$$t.c > .c4lc_pp.c 2>/dev/null; \
		$(C4CC) -o .c4lc_a.c4r .c4lc_pp.c > /dev/null 2>&1 || exit 1; \
		./c4m load-c4r.c -- .c4lc_a.c4r > .c4lc_out_a 2>&1; \
		./c4sp src/c4sp/lisp/c4lc.lisp .c4lc_pp.c .c4lc_b.c4r > /dev/null || exit 1; \
		./c4m load-c4r.c -- .c4lc_b.c4r 2>&1 | cmp - .c4lc_out_a || exit 1; \
	done
	# switch images must roundtrip and survive the optimizer (the
	# jumptable's dcode label targets move with the code)
	./c4sp src/c4sp/lisp/c4lc.lisp src/tests/test_switch.c .c4lc_b.c4r > /dev/null
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp .c4lc_b.c4r | grep -q "roundtrip identical"
	./c4sp src/c4sp/lisp/c4opt-run.lisp .c4lc_b.c4r .c4lc_bo.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_bo.c4r | cmp - src/c4sp/tests/expected/test_switch.txt
	# L4: -O runs the c4opt passes in-process (no intermediate file).
	# The optimized image differs from the two-step pipeline's only in
	# patch-covered operand words -- dead values the loader overwrites
	# -- so the bar is identical behavior; the tail pass lets a million
	# mutual tail calls run flat with no separate optimizer step.
	./c4sp src/c4sp/lisp/c4lc.lisp -O src/tests/c4lc_l2.c .c4lc_b.c4r | grep -q "tree: folded"
	$(C4CC) -o .c4lc_a.c4r src/tests/c4lc_l2.c
	./c4m load-c4r.c -- .c4lc_a.c4r > .c4lc_out_a
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - .c4lc_out_a
	./c4sp src/c4sp/lisp/c4lc.lisp -O src/tests/test_switch.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/test_switch.txt
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp .c4lc_b.c4r | grep -q "roundtrip identical"
	./c4sp src/c4sp/lisp/c4lc.lisp -O src/tests/test_tailcall.c .c4lc_b.c4r | grep -q " tail 2"
	./c4m load-c4r.c -- .c4lc_b.c4r | grep -q "parity 0 counter 1000000"
	# L7: structs, unions, typedef, member access, do-while, compound
	# assignment, block-scoped declarations with expression
	# initializers. gcc generated the expected output; c4cc cannot
	# compile any of this.
	./c4sp src/c4sp/lisp/c4lc.lisp src/tests/c4lc_l7.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-l7.txt
	./c4sp src/c4sp/lisp/c4lc.lisp -O src/tests/c4lc_l7.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-l7.txt
	./c4sp src/c4sp/lisp/c4r-roundtrip.lisp .c4lc_b.c4r | grep -q "roundtrip identical"
	# L11: integer constant expressions in enum bodies. p:const already
	# resolved enum names and already backed array sizes, case labels
	# and initializers; the enum parser just never used it. gcc is the
	# oracle -- c4cc's enum parser takes literal numbers only, so it
	# cannot compile this and the differential battery cannot hold it.
	./c4sp src/c4sp/lisp/c4lc.lisp $(TESTS)/c4lc_enum.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-enum.txt
	./c4sp src/c4sp/lisp/c4lc.lisp -O $(TESTS)/c4lc_enum.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-enum.txt
	# L10: -conforming escapes. The token golden is the tight check --
	# it pins the lexer directly instead of a program's output. Then
	# the runtime oracle, and then the SAME source WITHOUT the flag,
	# which must still print the c4cc quirks: an opt-in flag that is
	# always on is not opt-in, and nothing else would catch that.
	./c4sp src/c4sp/lisp/c4lc-tokens.lisp -conforming $(TESTS)/c4lc_lex_sample.c \
		| cmp - src/c4sp/tests/expected/c4lc-tokens-conforming.txt
	./c4sp src/c4sp/lisp/c4lc.lisp -conforming $(TESTS)/c4lc_esc.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-esc.txt
	./c4sp src/c4sp/lisp/c4lc.lisp -O -conforming $(TESTS)/c4lc_esc.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-esc.txt
	./c4sp src/c4sp/lisp/c4lc.lisp -O $(TESTS)/c4lc_esc.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-esc-quirks.txt
	# L8: object mode. -c leaves undefined prototypes as SYMBOL-typed
	# patches with extern symbol entries for c4rlink. c4lc objects link
	# with c4cc objects in either direction; an L7 struct program built
	# from separately compiled -O objects matches both the whole-program
	# compile and committed gcc output.
	$(C4CC) -o .c4lc_oa1.c4o $(TESTS)/test_link_a.c > /dev/null 2>&1
	$(C4CC) -o .c4lc_ob1.c4o $(TESTS)/test_link_b.c > /dev/null 2>&1
	$(C4RLINK) .c4lc_oa1.c4o .c4lc_ob1.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r > .c4lc_out_a 2>&1
	./c4sp src/c4sp/lisp/c4lc.lisp -c $(TESTS)/test_link_a.c .c4lc_oa2.c4o > /dev/null
	./c4sp src/c4sp/lisp/c4lc.lisp -c $(TESTS)/test_link_b.c .c4lc_ob2.c4o > /dev/null
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r 2>&1 | cmp - .c4lc_out_a
	$(C4RLINK) .c4lc_oa1.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r 2>&1 | cmp - .c4lc_out_a
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob1.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r 2>&1 | cmp - .c4lc_out_a
	./c4sp src/c4sp/lisp/c4lc.lisp -O -c $(TESTS)/test_link_c.c .c4lc_oa2.c4o > /dev/null
	./c4sp src/c4sp/lisp/c4lc.lisp -O -c $(TESTS)/test_link_d.c .c4lc_ob2.c4o > /dev/null
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-link.txt
	cat $(TESTS)/test_link_d.c $(TESTS)/test_link_c.c > .c4lc_pp.c
	./c4sp src/c4sp/lisp/c4lc.lisp .c4lc_pp.c .c4lc_ol.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-link.txt
	# L8, extern DATA: test_link_e.c defines shared globals (scalar,
	# array, char array, struct, fn-address slot), test_link_f.c
	# declares them extern; symbol patches resolve to DATA patches.
	# Linked both orders, -O objects, and the whole-program concat
	# (extern before definition) all match committed gcc output.
	./c4sp src/c4sp/lisp/c4lc.lisp -c $(TESTS)/test_link_e.c .c4lc_oa2.c4o > /dev/null
	./c4sp src/c4sp/lisp/c4lc.lisp -c $(TESTS)/test_link_f.c .c4lc_ob2.c4o > /dev/null
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-extdata.txt
	$(C4RLINK) .c4lc_ob2.c4o .c4lc_oa2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-extdata.txt
	./c4sp src/c4sp/lisp/c4lc.lisp -O -c $(TESTS)/test_link_e.c .c4lc_oa2.c4o > /dev/null
	./c4sp src/c4sp/lisp/c4lc.lisp -O -c $(TESTS)/test_link_f.c .c4lc_ob2.c4o > /dev/null
	$(C4RLINK) .c4lc_oa2.c4o .c4lc_ob2.c4o -o .c4lc_ol.c4r
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-extdata.txt
	cat $(TESTS)/test_link_f.c $(TESTS)/test_link_e.c > .c4lc_pp.c
	./c4sp src/c4sp/lisp/c4lc.lisp .c4lc_pp.c .c4lc_ol.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_ol.c4r | cmp - src/c4sp/tests/expected/c4lc-extdata.txt
	rm -f .c4lc_oa1.c4o .c4lc_ob1.c4o .c4lc_oa2.c4o .c4lc_ob2.c4o .c4lc_ol.c4r
	# L9: c4lc's own preprocessor. First the feature battery against
	# committed gcc-verified output (includes, object- and
	# function-like macros, line continuation, nesting, #ifdef/#ifndef
	# /#if/#elif/#else/#endif with constant expressions, #undef).
	./c4sp src/c4sp/lisp/c4lc.lisp -P $(TESTS)/c4lc_pp.c .c4lc_b.c4r > /dev/null
	./c4m load-c4r.c -- .c4lc_b.c4r | cmp - src/c4sp/tests/expected/c4lc-pp.txt
	# Then the property that matters: preprocessing a real module
	# with c4lc instead of gcc -E must produce the SAME OBJECT, byte
	# for byte. If that holds, gcc is no longer in the pipeline.
	./c4sp src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix src/c4ix/vfs.c .c4lc_oa2.c4o > /dev/null
	$(PREPROC) src/c4ix/vfs.c > .c4lc_pp.c
	./c4sp src/c4sp/lisp/c4lc.lisp -O -c .c4lc_pp.c .c4lc_ob2.c4o > /dev/null
	cmp .c4lc_oa2.c4o .c4lc_ob2.c4o
	rm -f .c4lc_oa2.c4o .c4lc_ob2.c4o
	# L5, the bootstrap battery: c4lc -O compiles the interpreter it
	# runs on, and the result must run the Lisp samples (call/cc
	# exercises the CEK machine), the byte-level c4r roundtrip, and
	# c4lc's own parser.
	$(PREPROC) src/c4sp/c4sp.c > .c4lc_pp.c
	./c4sp src/c4sp/lisp/c4lc.lisp -O .c4lc_pp.c .c4lc_sp.c4r > /dev/null
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
	./c4sp src/c4sp/lisp/c4lc.lisp src/tests/c4lc_l2.c .c4lc_ref.c4r > /dev/null
	./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/c4lc-eq.lisp src/tests/c4lc_l2.c .c4lc_ref.c4r | grep -q "identical"
	./c4m load-c4r.c -- .c4lc_sp.c4r src/c4sp/lisp/c4lc-eq.lisp src/tests/c4lc_l2.c .c4lc_ref.c4r | grep -q "identical"
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
	# The two evaluators must agree. Every build rule above runs c4lc under
	# -R (the recursive evaluator) rather than the CEK machine, because
	# c4lc uses neither call/cc nor first-class environments and so pays
	# CEK's per-call arena allocation for nothing. That is only safe while
	# the two produce the same image, so pin it here on a source that
	# exercises the whole pipeline, with and without -O.
	./c4sp    src/c4sp/lisp/c4lc.lisp -O src/tests/c4lc_l7.c .c4lc_cek.c4r > /dev/null
	./c4sp -R src/c4sp/lisp/c4lc.lisp -O src/tests/c4lc_l7.c .c4lc_rec.c4r > /dev/null
	cmp .c4lc_cek.c4r .c4lc_rec.c4r
	./c4sp    src/c4sp/lisp/c4lc.lisp src/tests/c4lc_l7.c .c4lc_cek.c4r > /dev/null
	./c4sp -R src/c4sp/lisp/c4lc.lisp src/tests/c4lc_l7.c .c4lc_rec.c4r > /dev/null
	cmp .c4lc_cek.c4r .c4lc_rec.c4r
	rm -f .c4lc_cek.c4r .c4lc_rec.c4r
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
	# Linking in the machine: two objects compiled to the RAM filesystem,
	# LINKED from it, and the result run out of it. This is the pin for
	# building C4IX in-machine -- it is twelve objects plus a library.
	$(C4M) $(RUN_C4KE) test_ramlink | grep -q "b_add(3, 4) = 7"
	@echo "test-c4ke-ramfs: OK"

# --- Task memory (docs/task-memory.md) --------------------------------
#
# A task's malloc() only reaches the kernel when protected mode is on,
# and protected mode is not the default, so both of these build their
# own kernel rather than using the shipped one.
c4ke-pm.c4r: $(C4CC) $(C4KE_SRCS)
	$(PREPROC) -DCONFIG_ENABLE_PM=1 src/c4ke/c4ke.c | $(C4CC) -o $@ -
c4ke32-pm.c4r: c4cc32 $(C4KE_SRCS)
	$(PREPROC) -DCONFIG_ENABLE_PM=1 src/c4ke/c4ke.c | ./c4cc32 -o $@ -
c4ke32-nopm.c4r: c4cc32 $(C4KE_SRCS)
	$(PREPROC) src/c4ke/c4ke.c | ./c4cc32 -o $@ -

# test_leak allocates 6 MB in each of twelve tasks and frees none of it,
# on purpose, and each round reports how much it got. On a 32 MB
# breadboard, 72 MB of leak can only run to the end if the kernel takes
# each task's memory back when the task ends -- so the control run with
# the same test on a kernel WITHOUT protected mode is part of the test:
# it must fail, or the passing run proves nothing.
test-task-mem: c4ke-pm.c4r c4ke32-pm.c4r c4ke32-nopm.c4r $(C4M) $(C4CC) c4cc32 $(TESTS)/test_leak.c
	$(C4CC) -o .tm_leak.c4r $(U0) $(TESTS)/test_leak.c
	$(C4M) load-c4r.c -- c4ke-pm.c4r .tm_leak.c4r | grep -q "test_leak: 12 rounds done"
	@mkdir -p .tm_disk
	./c4cc32 -o .tm_disk/test_leak.c4r $(U0) $(TESTS)/test_leak.c
	node src/c4bb/sim/cli.js -m 32 -d .tm_disk c4ke32-pm.c4r test_leak.c4r \
	  | grep -q "test_leak: 12 rounds done"
	# The control. Without the tracking, the machine runs out partway.
	! node src/c4bb/sim/cli.js -m 32 -d .tm_disk c4ke32-nopm.c4r test_leak.c4r \
	  | grep -q "test_leak: 12 rounds done"
	@rm -rf .tm_disk .tm_leak.c4r
	@echo "test-task-mem: OK -- 72 MB of leak in a 32 MB machine, and the"
	@echo "                     same test failing without the tracking"

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
	rm -rf $(C4RS) $(BIN) *.c4r c4ke.pre.c .tm_disk .tm_leak.c4r
clean: clean-c4rs
	rm -rf $(C4) $(C4M) $(C4CC) c4mp
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
PHONY += test-c4tui test-c4th-bb run-c4dos-c4fc test-c4dos-c4fc
PHONY += run-c4dos-build32 test-c4dos-build32 run-c4dos-c4ix32 test-c4dos-c4ix32 test-c4cc-for test-respfile test-b4ke test-c4bb-storage test-c4bb-baseops test-c4bb-rungs test-c4bb-install test-c4bb-climb test-c4bb-web test-c4bb-firmware
PHONY += test-c4sc test-c4sc-run test-c4sc-lex test-c4sc-front test-c4sc-back
PHONY += test-c4sc-image test-c4sc-self test-c4sc-board
PHONY += test-c4dos-ladder32 serve-c4bb
PHONY += run run-vg test test-massive
PHONY += run-alt run-alt-vg test-alt test-massive-alt
PHONY += run-c4 run-c4-vg test-c4 test-massive-c4
PHONY += run-c4-alt run-c4-alt-vg
PHONY += test-c4ix test-c4ix-fmt test-c4ix-c4ke test-c4ix-c4ke-nested test-c4ix-c4 run-c4ix run-c4ix-c4 demo-c4ix demo-c4ix-c4 bench-c4ix
PHONY += test-c4mp
PHONY += test-c4m-mem
PHONY += test-c4th
PHONY += c4or1k-m0 c4or1k-m1 c4or1k-m1-check c4or1k-m2 c4or1k-m2-check c4or1k-m3 c4or1k-m3-check c4or1k-m3-int-check c4or1k-boot c4or1k-boot-mp c4or1k-boot-cisc c4or1k-boot-jit c4or1k-boot-native
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
	gcc $(EXTRA_CC) -O2 -fwrapv -g -idirafter include -I . c4m.c c4m_float.c -o c4m -lm
# The B5c opcode probe's measuring half -- see docs/c4th-design.md. It
# generates an instrumented copy of c4m rather than living in c4m.c,
# because plain c4 compiles both arms of an #ifdef (c4.c:74 skips only
# the "#" line), and the probe is not C4-subset C.
c4m-fuse: c4m.c c4cc.c4r c4sp.c4r
	sh src/c4th/bench/fuse-probe.sh
c4cc: $(C4CC_SRCS)
	$(call compile_c,src/c4cc/asm-c4r.c,c4cc)
# c4rdump compiles c4cc.c in for its instruction name table, so it
# has to rebuild when that table changes.
$(C4RDUMP): src/c4ke/bin/c4rdump.c load-c4r.c src/c4cc/c4cc.c
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
# The extensions are #included by c4ke.c, so a change to one has to
# rebuild the kernel -- C4KE_SRCS names them all.
$(C4KE_C4R): $(C4CC) $(C4KE_SRCS)
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
# raycast, the console raycaster (src/tests/raycast.c).
#
# Exclusive rule, and c4lc rather than c4cc, for three reasons:
#   - -conforming, so ANSI escapes can be written as "\033[" instead of
#     poked in as integers the way mandel.c has to;
#   - no u0, because c4lc's preprocessor hangs on u0.h (see the vfsload
#     note in src/c4bb/tests/build-images.sh) and the program needs
#     nothing from it -- puts/__time/__c4_cycles are c4lc builtins;
#   - -D, which is also what turns the preprocessor ON. Compiled with
#     no -D at all, BOTH sides of the RC_DOS #ifdef would be compiled.
$(TESTS)/raycast.c4r: c4sp $(C4LC_LISP) $(TESTS)/raycast.c
	./c4sp src/c4sp/lisp/c4lc.lisp -O -conforming -D RC_KE=1 \
		$(TESTS)/raycast.c $@ > /dev/null

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
