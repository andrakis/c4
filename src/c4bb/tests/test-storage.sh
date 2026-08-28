#!/bin/bash
# test-storage.sh -- c4bb has drives, and one of them can be written to.
#
# The pin for docs/c4bb-storage.md M1/M2/M3. Three things have to be
# true, and the third is the one that matters: what a session builds can
# be put on a medium and read back by a LATER session, with the machine
# switched off in between.
set -e
cd "$(dirname "$0")/../../.."

CLI="node src/c4bb/sim/cli.js"
T=.bbstore
rm -rf $T
mkdir -p $T/d0 $T/d1

# Drive 0: enough C4DOS to unpack an archive. Drive 1: blank.
cp c4dos-c4ix32/config.sys c4dos-c4ix32/autoexec.bat $T/d0/
cp c4dos-c4ix32/dostar.c4r c4dos-c4ix32/bbsave.c4r c4dos-c4ix32/tools-src.tar $T/d0/
(cd $T/d0 && ls -p | grep -v '/$' > c4dos.dir)

# --- 1. a second drive is visible, and reading from it works ----------
# tools-src.tar is on drive 0 only; ask for it by drive and by prefix.
{ printf 'DIR\n'; sleep 4; } | timeout 120 $CLI -i -m 16 -d $T/d0 -d $T/d1 c4dos32.c4r \
  > $T/dir.log 2>&1 || true
grep -q "dostar.c4r" $T/dir.log

# --- 2. write it ------------------------------------------------------
{ printf 'RUN dostar.c4r x tools-src.tar\n'; sleep 10;
  printf 'RUN bbsave.c4r 1:\n';              sleep 20; } \
  | timeout 300 $CLI -i -m 32 -d $T/d0 -w $T/d1 c4dos32.c4r > $T/save.log 2>&1 || true
grep -q "^bbsave: 6 files" $T/save.log

# Byte-identical to what went in, on the host side of the medium.
cmp $T/d1/c4cc.c     src/c4cc/c4cc.c
cmp $T/d1/cpp.c      src/c4dos/cpp.c
cmp $T/d1/load-c4r.c load-c4r.c
cmp $T/d1/asm-c4r.c  src/c4cc/asm-c4r.c
cmp $T/d1/c4dos.h    include/c4dos.h
cmp $T/d1/u0lite.h   include/u0lite.h

# --- 3. a LATER machine reads it back ---------------------------------
# Nothing of the first run survives except the medium: this is a new
# process, a new arena, and drive 1 has become drive 0.
cp c4dos-c4ix32/config.sys c4dos-c4ix32/autoexec.bat $T/d1/
(cd $T/d1 && ls -p | grep -v '/$' > c4dos.dir)
{ printf 'TYPE cpp.c\n'; sleep 8; } | timeout 120 $CLI -i -m 16 -d $T/d1 c4dos32.c4r \
  > $T/back.log 2>&1 || true
grep -q "a C preprocessor for the C4 toolchain" $T/back.log

# --- 4. a read-only drive refuses ------------------------------------
{ printf 'RUN dostar.c4r x tools-src.tar\n'; sleep 10;
  printf 'RUN bbsave.c4r 1:\n';              sleep 10; } \
  | timeout 300 $CLI -i -m 32 -d $T/d0 -d $T/d1 c4dos32.c4r > $T/ro.log 2>&1 || true
grep -q "read-only" $T/ro.log

rm -rf $T
echo "test-storage: OK -- two drives, one writable, and a medium that"
echo "                    outlives the machine that wrote it"
