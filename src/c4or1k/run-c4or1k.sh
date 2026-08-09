#!/bin/bash
# run-c4or1k.sh -- put the terminal in raw mode, run the emulator,
# always restore the terminal on exit.
#
# The C4 VM has no ioctl/termios facility (confirmed absent from
# c4.c/c4m.c repo-wide): READ/WRITE are thin wrappers over the host's
# read()/write(), so the VM just inherits whatever termios state fd 0
# already has. Putting fd 0 in raw mode is therefore an *external*,
# host-side concern -- this script -- not something c4or1k itself can
# or should do. This is the same division of responsibility as
# src/c4ix/console.c, which assumes a cooked tty on fd 0 by default and
# works around it at the OS layer (non-blocking re-open), not the VM
# layer.
#
# Usage: run-c4or1k.sh [c4or1k.c4r args...]

set -e
cd "$(dirname "$0")/../.."

restore() {
    stty sane 2>/dev/null || true
}
trap restore EXIT INT TERM


# Not fatal when stdin isn't a real tty (piped input, CI, this repo's
# own test scripts) -- raw mode is meaningless there anyway, and the
# emulator should still run against whatever bytes arrive on fd 0.
stty raw -echo 2>/dev/null || true

exec ./c4m load-c4r.c -- "$@"
