# C4 and suite Makefile
#
# Target 'all' builds:
#   c4, c4m, c4cc, c4rdump, c4rlink executables
#   uses c4cc to build the various .c4r files
#
# Make targets:
#   run
#   run-c4ke      Run C4KE under c4m (fastest)
#   run-c4        Run C4KE under c4, running under c4m (slowest)
#   run-alt       Alternate invocations for c4m, fixing a timing issue
#   run-c4ke-alt  ..
#   run-c4-alt    ..
#   run-vg        Run C4KE under valgrind
#   run-c4ke-vg
#   run-c4-vg
#   c4rs          Compile .c4r files
#   test          Test C4KE using the 'innerbench' tool
#   test-massive  Test C4KE using 'innerbench' and a large number of
#                 child processes.

C4          := ./c4
C4M         := ./c4m
C4M_C       := $(C4M).c
C4KE_BOOT   :=
C4KE_INVOC  := load-c4r.c -- c4ke $(C4KE_BOOT)

CC          := gcc
C4CC        := ./c4cc
C4CC_SRCS   := asm-c4r.c
CCOR1K      := /opt/or1k-linux-musl/bin/or1k-musl-linux-gcc
CFLAGS      := -g -O2 -Wno-format -ggdb3
CFLAGSOR1K  := -DOPENRISC
DEPFLAGS     =

LDFLAGS     :=
LDLIBS      :=

VPATH       := ./
# Compile c4, c4m, and c4cc separately
OBJS        := c4.o c4m.o c4cc.o
C4R_SRCS    := c4.c4r c4ke.c4r c4ke.vfs.c4r c4m.c4r classes_test.c4r factorial.c4r \
               hello.c4r load-c4r.c4r multifun.c4r test-oisc.c4r test-order.c4r \
			   test-ptrs.c4r test_args.c4r test_basic.c4r test_customop.c4r \
			   test_fread.c4r test_malloc.c4r test_printf.c4r tests.c4r \
			   oisc4.c4r test_printloop.c4r c4cc.c4r \
			   test_exit.c4r test_infiniteloop.c4r \
			   top.c4r ps.c4r bench.c4r echo.c4r \
			   ls.c4r c4rdump.c4r c4rlink.c4r xxd.c4r benchtop.c4r innerbench.c4r \
			   type.c4r \
			   init.c4r \
               c4sh.c4r \
			   test_signal.c4r spin.c4r
C4KE_SRCS   := load-c4r.c c4ke.c c4ke_ipc.c c4ke_plus.c
U0          := u0.c
U0_LIB      := u0.c4l
C4R_LIBS    := $(U0_LIB)
OISC4_SRCS  := oisc-min.c oisc-asm.c oisc-c4.c
ESHELL_SRCS := ps.c eshell.c
C4CC_SRCS   := load-c4r.c c4cc.c asm-c4r.c
C4KE_VFS_SRCS := $(U0) service.h c4ke.vfs.c
# If any of these files change, .c4r files should be rebuilt
C4R_WATCH   := $(U0) $(C4KE_SRCS)
C4RDUMP     := c4rdump
C4RLINK     := c4rlink
C4RUTILS    := $(C4RDUMP) $(C4RLINK)
C4SH_SRCS   := u0.c ps.c c4sh.c c4sh_builtins.c c4sh_scripting.c
INIT_SRCS   := u0.c ps.c eshell.c init.c

.PHONY: all clean-c4rs clean test bits run-c4ke

# TODO: Disabled: $(C4RUTILS)
all: $(C4) $(C4M) $(C4CC) | c4rs

# This target builds the .c4r files
c4rs: $(C4CC) $(C4R_LIBS) | $(C4R_SRCS)

# This target runs the C4 Kernel Experiment
run-c4ke: $(C4R_SRCS) $(C4KE_SRCS) init.c4r c4ke.vfs.c4r
	$(C4M) $(C4KE_INVOC) $(C4KE_BOOT)
run-c4ke-alt: $(C4R_SRCS) $(C4KE_SRCS) init.c4r c4ke.vfs.c4r
	$(C4M) -a $(C4KE_INVOC) $(C4KE_BOOT)
run-c4ke-vg: $(C4R_SRCS) $(C4KE_SRCS) init.c4r c4ke.vfs.c4r
	valgrind $(C4M) $(C4KE_INVOC) 
run-c4ke-vg-alt: $(C4R_SRCS) $(C4KE_SRCS) init.c4r c4ke.vfs.c4r
	valgrind $(C4M) -a $(C4KE_INVOC) 
run-c4-c4ke: $(C4) $(C4R_SRCS) $(C4KE_SRCS) init.c4r c4ke.vfs.c4r
	$(C4) $(C4M_C) $(C4KE_INVOC) $(C4KE_BOOT)
run-c4-c4ke-alt: $(C4) $(C4R_SRCS) $(C4KE_SRCS) init.c4r c4ke.vfs.c4r
	$(C4) $(C4M_C) -a $(C4KE_INVOC) $(C4KE_BOOT)

# Aliases
run: run-c4ke
run-alt: run-c4ke-alt
run-vg: run-c4ke-vg
run-vg-alt: run-c4ke-vg-alt
run-c4: run-c4-c4ke
run-c4-alt: run-c4-c4ke-alt

$(C4RDUMP): $(C4CC_SRCS) c4rdump.c
	$(CC) $(CFLAGS) -g c4rdump.c -o c4rdump
$(C4RLINK): $(C4CC_SRCS) c4rlink.c
	$(CC) $(CFLAGS) -g c4rlink.c -o c4rlink

clean-c4rs:
	$(RM) $(C4R_SRCS) c4ke.c4r c4cc.c4r
clean: clean-c4rs
	$(RM) $(OBJS) $(C4) $(C4M) $(C4CC) $(C4RUTILS)

# Rules for building C4 Relocatables (.c4r)
# All of these, except for C4KE itself, include u0.c in their compilation.
# Specific rules have multiple source files required. At the end is the generic rule.
hello.c4r: hello.c
	# We specifically do not use u0.c here
	$(C4CC) -o hello.c4r hello.c
c4ke.c4r: $(C4KE_SRCS) $(C4CC)
	$(C4CC) -o c4ke.c4r $(C4KE_SRCS)
classes_test.c4r: $(U0) classes.c classes_test.c $(C4R_WATCH) $(C4CC)
	$(C4CC) -o classes_test.c4r $(U0) classes.c classes_test.c
oisc4.c4r: $(OISC4_SRCS) $(C4CC) $(C4R_WATCH)
	$(C4CC) -o oisc4.c4r $(U0) $(OISC4_SRCS)
c4cc.c4r: c4cc.c asm-c4r.c $(C4R_WATCH) $(C4CC)
	$(C4CC) -o c4cc.c4r $(U0) c4cc.c load-c4r.c asm-c4r.c
c4ke.vfs.c4r: $(C4KE_VFS_SRCS) $(C4R_WATCH) $(C4CC)
	$(C4CC) -o c4ke.vfs.c4r $(C4KE_VFS_SRCS)
#eshell.c4r: $(ESHELL_SRCS) $(C4R_WATCH) $(C4CC)
#	$(C4CC) -o eshell.c4r $(U0) $(ESHELL_SRCS)
top.c4r: ps.c top.c $(U0) $(C4R_WATCH) $(C4CC)
	$(C4CC) -o top.c4r $(U0) ps.c top.c
c4rdump.c4r: c4rdump.c $(C4CC_SRCS) $(C4CC) $(CAR_WATCH)
	$(C4CC) -o c4rdump.c4r $(U0) $(C4CC_SRCS) c4rdump.c
c4rlink.c4r: c4rlink.c $(C4CC_SRCS) $(C4CC) $(CAR_WATCH)
	$(C4CC) -o c4rlink.c4r $(U0) $(C4CC_SRCS) c4rlink.c
c4sh.c4r: $(C4SH_SRCS) $(C4CC)
	$(C4CC) -o c4sh.c4r $(C4SH_SRCS)
init.c4r: $(INIT_SRCS) $(C4CC)
	$(C4CC) -o init.c4r $(INIT_SRCS)
# Generic rules that work for single-source-file programs
%.c4r: %.c $(C4R_WATCH) $(C4CC)
	$(C4CC) -o $@ $(U0) $<
# ...and libraries
%.c4l: %.c $(C4R_WATCH) $(C4CC)
	$(C4CC) -o $@ $<

# Rules for building c4, c4m, c4cc, c4p
%.o: %.c %.d
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

c4cc.o: $(C4CC_SRCS)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c asm-c4r.c -o c4cc.o

c4: c4.o
	$(CC) $(CFLAGS) $(LDFLAGS) c4.o -o $@ $(LDLIBS)

c4m: c4m.o
	$(CC) $(CFLAGS) $(LDFLAGS) c4m.o -o $@ $(LDLIBS)

c4cc: c4cc.o
	$(CC) $(CFLAGS) $(LDFLAGS) c4cc.o -o $@ $(LDLIBS)

test: $(C4M) $(C4KE_SRCS)
	$(C4M) $(C4KE_INVOC) innerbench
test-massive: $(C4M) $(C4KE_SRCS)
	# -B      invoke 'bench' directly instead of via c4m
    # -n 40   invoke 40 copies
	$(C4M) $(C4KE_INVOC) innerbench -Bn 40

# Personal testing section
OR1K_TGZ := c4-or1k-new.tgz
DEST_TGZ := ~/git/jorconsole/jor1k-sysroot/fs/home/user/c4.tgz
# Rule for building for OpenRISC1000. Personal testing.
or1k: clean
	# Cross-compile the various executables
	$(CCOR1K) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGSOR1K) -c c4.c -o c4.o
	$(CCOR1K) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGSOR1K) -c c4m.c -o c4m.o
	$(CCOR1K) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGSOR1K) -c asm-c4r.c -o c4cc.o
	$(CCOR1K) $(CFLAGS) $(LDFLAGS) c4.o $(LDLIBS) -o c4
	$(CCOR1K) $(CFLAGS) $(LDFLAGS) c4m.o $(LDLIBS) -o c4m
	$(CCOR1K) $(CFLAGS) $(LDFLAGS) c4cc.o $(LDLIBS) -o c4cc
	tar cjf $(OR1K_TGZ) c4ke.vfs.txt $(C4) *.c *.h $(C4M) $(C4CC) stdlib/*.c stdlib/*.h c4ke Makefile or1k.sh *.o
	cp $(OR1K_TGZ) $(DEST_TGZ)
PI_TGZ=pi.tgz
# Rule for building for a test machine, a raspberry pi
pi:
	tar cjf $(PI_TGZ) c4ke.vfs.txt *.c *.h stdlib/*.c stdlib/*.h c4ke Makefile
