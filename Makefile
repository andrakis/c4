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
NATIVE_CC      := gcc
NATIVE_CC_OPTS := -O2 -g -idirafter include -I .
NATIVE_TARGETS := c4 c4m c4cc
C4        := ./c4
C4M       := ./c4m
C4CC      := ./c4cc
SRCS      := src
INCLUDE   := include
C4CC_SRCS := $(SRCS)/c4cc/c4cc.c $(SRCS)/c4cc/asm-c4r.c
# Version of C4CC compiled to .c4r format
C4R_C4CC_SRCS := $(U0) $(SRCS)/c4cc/c4cc.c load-c4r.c $(SRCS)/c4cc/asm-c4r.c
C4KE_SRCS := load-c4r.c $(SRCS)/c4ke/c4ke.c \
             $(SRCS)/c4ke/extensions/c4ke_ipc.c $(SRCS)/c4ke/extensions/c4ke_plus.c
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
			 $(TESTS)/test_signal.c4r $(TESTS)/test_static.c4r $(TESTS)/tests.c4r
BIN       := $(C4R_C4CC) $(C4R_C4RDUMP) $(C4R_C4RLINK) $(C4R_TOP) \
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
	$(NATIVE_CC) $(NATIVE_CC_OPTS) ${$@_source} -o ${$@_out}
endef

# Prerequisites: the native C4 and related binaries, plus C4KE, and supporting binaries
pre: $(C4) $(C4M) $(C4CC) $(C4RS) $(BIN_D) $(BENCHS)
	cp $(BIN_D)/*.c4r .
	cp $(SRCS)/bench/*.c4r .
	cp $(SRCS)/tests/*.c4r .

# Standard targets
all: pre

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
clean:
	rm -rf $(C4) $(C4M) $(C4CC) $(C4RS) $(BIN) *.c4r
pkg:
	tar cjf $(PKG) c4ke.vfs.txt *.c src include Makefile

# Marking the above rules as PHONY using singular .PHONY rule
PHONY  = pre all clean
PHONY += run run-vg test test-massive
PHONY += run-alt run-alt-vg test-alt test-massive-alt
PHONY += run-c4 run-c4-vg test-c4 test-massive-c4
PHONY += run-c4-alt run-c4-alt-vg
PHONY += pkg
.PHONY: $(PHONY)

#
# Rules to build native versions of c4, c4m, and c4cc
#
c4: c4.c
	$(call compile_c,$<,$@)
c4m: c4m.c
	$(call compile_c,$<,$@)
c4cc: $(C4CC_SRCS)
	$(call compile_c,src/c4cc/asm-c4r.c,c4cc)

#
# Rules to build C4R files
#

c4m.c4r: c4m.c include/c4m.h $(C4CC)
	$(C4CC) -o c4m.c4r c4m.c

# C4KE - The C4 Kernel Experiment, and supporting files
$(C4KE_C4R): $(C4CC) $(C4KE_SRCS) $(C4KE_HDRS)
	$(C4CC) -o $(C4KE_C4R) $(C4KE_SRCS)
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
$(C4R_C4CC): $(C4CC) $(U0) $(SRCS)/c4cc/c4cc.c load-c4r.c $(SRCS)/c4cc/asm-c4r.c
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
	$(C4CC) -o hello.c4r $(TESTS)/hello.c
# All tests should compile with the following invocation
$(SRCS)/tests/%.c4r: $(SRCS)/tests/%.c $(C4KE_WATCH) $(C4CC)
	$(C4CC) -o $@ $(U0) $<
# Build the u0 library for linking.
# TODO: c4rlink doesn't support linking yet.
%.c4l: %.c $(C4KE_WATCH) $(C4CC)
	$(C4CC) -o $@ $<

