#!/usr/bin/env python3
# qser.py SOCK LOG STEP... -- drive QEMU's COM1, a unix socket (QEMU the
# server: -serial unix:SOCK,server=on,wait=off), for the tests that run
# c4pc.elf in QEMU (test-c4pc-net.sh). Each STEP is "expect:TEXT" (wait for
# TEXT, QSER_T seconds at most, 120 if not said), "send:TEXT" (\n, \r, \e
# and \d, a down arrow, understood; a byte each 10 ms, as typing), or
# "sleep:S". Everything read goes to LOG. Exit 1 on a timeout, 0 when the
# steps are done and QEMU closed the line (or 120 s more passed).
import socket, sys, time, os

path, log = sys.argv[1], open(sys.argv[2], "wb")
limit = float(os.environ.get("QSER_T", "120"))
for _ in range(100):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(path); break
    except OSError:
        time.sleep(0.1)
s.settimeout(0.2)
seen = b""

def pump():
    global seen
    try:
        d = s.recv(65536)
    except socket.timeout:
        return True
    if not d:
        return False
    log.write(d); log.flush()
    seen += d
    return True

def unesc(t):
    return t.replace("\\n", "\n").replace("\\r", "\r").replace("\\e", "\x1b").replace("\\d", "\x1b[B").encode()

for step in sys.argv[3:]:
    kind, _, arg = step.partition(":")
    if kind == "expect":
        want = arg.encode()
        t0 = time.time()
        while want not in seen:
            if not pump() or time.time() - t0 > limit:
                print("qser: never saw %r" % arg); sys.stdout.flush(); sys.exit(1)
        seen = seen[seen.index(want) + len(want):]
    elif kind == "send":
        for b in unesc(arg):
            s.send(bytes([b])); time.sleep(0.01)
    elif kind == "sleep":
        t0 = time.time()
        while time.time() - t0 < float(arg):
            if not pump(): break
t0 = time.time()
while time.time() - t0 < 120 and pump():
    pass
