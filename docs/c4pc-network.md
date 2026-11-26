# Networking for C4IX on c4pc: the plan

Written 2026-11-26 for the user's question: *"Plan the networking feature.
I'm unsure if full TCP/IP is the way to go first and want to see what you
propose. Try to get a version working -- I'd like to see tools like `ping`
and some way to talk between VMs on the hypervisor. Outside networking not
needed yet."*

Branch `c4pc-net` (helios `~/git/c4-pc-net`, cut from c4pc 968eafa). A copy
of this file is `docs/c4pc-network.md` there, with what was built and
measured added as it went.

## The recommendation, in short

**Real protocols on a paravirtual card, from the first day -- but only the
small half of TCP/IP to start: Ethernet, ARP, IPv4, ICMP echo and UDP. TCP
second.**

- Not full TCP/IP first. TCP is most of a stack's code and nearly all of
  its subtlety (state machine, retransmission, windows, the timers), and
  nothing the user asked for -- `ping`, and VMs talking -- needs it: ICMP
  echo is ping, and UDP datagrams carry a chat or a `nc -u` session.
- Not a private VM-to-VM channel either. A pipe between two machines would
  be quicker to build by a day, then thrown away: it shares nothing with
  what real networking needs. Ethernet frames, ARP and IPv4 cost little
  (about 600 lines of c4 in the kernel), and with them the switch is an
  ordinary learning switch, frames can be dumped as pcap and read in
  Wireshark, and the day a real NIC is added the same frames go out of it.
- The guest's card is **paravirtual** (a window of registers, as the disks'
  BLK window is), not an emulated NE2000/RTL8139/e1000. An emulated card is
  device-model code in c4pc *and* libjs plus a harder driver in C4IX, and
  buys nothing for c4 guests: nobody runs another OS's driver against it.
  An emulated real card's register set is what the *PC* needs at the
  outside edge (c4pc driving the PC's e1000), and that is the host's
  driver, not the guest's (milestone N4).

## The layers, and where each lives

```
 C4IX userland     ping   ifconfig   nc -u   (talk = nc -u both ways)
                   ---- syscalls: SOCKET BIND SENDTO RECVFROM NETCTL ----
 C4IX kernel       net.c: sockets (fds, VN_SOCK), UDP, ICMP, IPv4, ARP,
                   loopback, the c4net driver (polled each ms, in the trap)
                   ---- page 0: the NET window, 0x580-0x59c (kernel's) ----
 c4 machine        c4net card: MAC, address, a queue of received frames
 (c4pc net.c)      ---- frames ----
 hypervisor        the switch (c4pc net.c): a port per machine, by MAC,
                   broadcast to all; wakes a napping machine on delivery
 later             an uplink port: the PC's NIC (e1000) bridged or NATed (N4);
                   libjs: a port over BroadcastChannel or a WebSocket (N2)
```

### The card: the NET window

Eight registers in every machine's page 0, the kernel's alone (the MMU's
IOPM gives user mode none of it, as with BLK), present when the machine has
a port (INFO's bit `C4I_NET`, 0x1000000):

| reg | | |
|---|---|---|
| 0x580 STATUS | r | 1 a port (link), 2 frames waiting |
| 0x584 MAC0 | r | the MAC's first four bytes (low byte first) |
| 0x588 MAC1 | r | its last two |
| 0x58c IP | r | the address the switch suggests (10.4.0.N+1 for VM N), 0 none |
| 0x590 ADDR | w | physical address for the next command |
| 0x594 LEN | w | its length |
| 0x598 CMD | w | 1 send LEN bytes at ADDR; 2 the next frame received into ADDR (LEN at most); 3 an address for the name at ADDR; r: the result (bytes, or the address; -1) |
| 0x59c RXQ | r | frames waiting |

A frame is copied on each command, as a disk's sectors are. Rings shared
with the host (virtio's way) would save the copy and the register writes;
at a few thousand frames a second they are not worth it yet, and the
window can grow a ring mode later without the stack changing.

### The switch

In c4pc's `net.c`: up to 16 ports, one per machine, each with a queue of 64
frames. A frame goes to the port whose MAC it names, or to every other
port when broadcast or unknown. Delivery wakes the machine if it naps (a
guest's nap ended as `vm send` ends it; VM 0's nap loop looks at its
queue). The switch has its own lock: machines on different processors send
at once. MACs are 52:54:00:c4:00:ID, addresses 10.4.0.(ID+1)/24 -- VM 0 is
10.4.0.1 -- and CMD 3 answers a machine's name (`ping one`) or `vmN`.

### The kernel

`src/c4ix/net.c`, one module: the driver (polled each millisecond of the
machine's clock where the scheduler looks at its queues -- a register read
when nothing waits), ARP (a table of 16, one packet held while it asks),
IPv4 (no options, no fragments: the MTU is 1500 and UDP payloads stop at
1472), ICMP (echo answered in the kernel; replies to ping's sockets), UDP,
and loopback (127.0.0.1 and the machine's own address never reach the
card). Sockets are file descriptors (a new vnode type, VN_SOCK): a task
blocks in RECVFROM as a pipe's reader does (the scheduler's poll queue),
with a deadline, and close() frees it.

Syscalls (three words each, as every C4IX syscall):

| | | |
|---|---|---|
| 258 SOCKET (kind) | 1 UDP, 2 ICMP echo | an fd |
| 259 BIND (fd, port) | a UDP port, or ICMP's ident (0: one is chosen) | the port |
| 260 SENDTO (fd, msg) | msg = {buf, len, ip, port} | bytes, or -1 (no route, ARP gave up) |
| 261 RECVFROM (fd, msg) | msg = {buf, len, ip out, port out, deadline ms (0 none, -1 don't wait)} | bytes, -1, -2 timed out |
| 262 NETCTL (op, a, b) | 0 interfaces as text into a (b bytes), 1 set the address (a ip, b mask bits), 2 resolve the name at a | |

### Userland

- `ifconfig` -- net0 (its MAC, address, mask, frames and bytes each way,
  drops) and lo; `ifconfig net0 10.4.0.9/24` sets it.
- `ping [-c N] [-i MS] [-s BYTES] HOST` -- Linux's output and summary
  (transmitted, received, loss, min/avg/max). HOST: a dotted address, a
  machine's name, or `vmN`.
- `nc -u HOST PORT` and `nc -u -l PORT` -- netcat's UDP mode: stdin's lines
  out as datagrams, what comes printed. Two machines each running one is
  the talk program; `echo hi | nc -u one 7000` a one-shot message.

### libjs, the browser machine

Nothing above is c4pc's alone except the switch: the NET window is plain
page-0 registers, so `libjs/net.js` would be the same eight registers
(frames to and from a JS port). Ports for libjs: two tabs of the same page
through a BroadcastChannel (two browser machines pinging each other with
no server at all), or serve.mjs as a switch over a WebSocket, and cli.js
over a UDP socket to another cli.js or c4pc-host -- the same frames, so a
browser machine and a c4pc guest could share one switch. The kernel is the
same kernel: it looks for `C4I_NET` and stays quiet without it. That is
milestone N2.

## Milestones

- **N1 (this session): host-only ping and talk.** The NET window and the
  switch in c4pc (hypervisor guests and VM 0), net.c in C4IX, `ping`,
  `ifconfig`, `nc`; a QEMU test (`src/c4pc/test-c4pc-net.sh`): VM 0 and two
  guests, ping each way, a datagram across, the counters. The ISO's steps
  below.
- **N2: the card outside the hypervisor.** c4pc-host `--net udp:PORT:PEER`
  (two hosts on one PC ping each other: a fast test with no QEMU); libjs
  net.js with a BroadcastChannel port and cli.js's UDP; Machines' Network
  tab (each port's counts in INFO).
- **N3: TCP.** connect/listen/accept, streams as fds (read and write, so
  `cat`, sh's redirections and pipes work over a connection),
  retransmission and a fixed window first; `nc` without -u, a `telnetd`
  giving a guest's shell to another machine, a small `httpd`; select/poll
  over sockets (uevwait learns them).
- **N4: the outside world.** c4pc drives the PC's NIC (e1000/82540EM: QEMU,
  VMware and VirtualBox all offer it; virtio-net after), and the switch
  gets an uplink port: NAT for the guests (simplest for the user: no
  addresses to hand out on the LAN) or a bridge; a DHCP client for VM 0, a
  DNS resolver; packet dumps (`vm netdump`, pcap).
- **N5**: the C4IX socket API in libc as BSD's (`socket`, `sendto`, ...),
  so ports that want networking (jor1k's, games' multiplayer) compile.

## Risks

- **Waking a napping machine.** C4IX naps up to 100 ms when idle; a frame
  must end the nap or ping times are 100 ms. Guests: the switch ends the
  nap as `vm send` does; VM 0: its nap loop checks the queue. Measured in
  N1's test.
- **Processors at once.** Guests run on processors of their own: the
  switch's queues are under its own lock, and each machine's card is only
  its kernel's (one command at a time under its own klock).
- **Copies and polls.** Per-frame register commands and a poll each ms are
  fine for ping and chat; bulk transfers (N3) may want rings and an
  interrupt (a hard IRQ trap) -- measure first.
- **No TCP yet** means no streams: `nc` is UDP only and a lost datagram is
  lost (the switch drops only when a queue of 64 is full).
- **c4's C** (c4lc): no unsigned arithmetic the stack can lean on; header
  fields are built a byte at a time with masks, and checksums in ints.
- **The shared files.** The CPU 0 session is changing `engine.c`, `hv.c`
  and `pc.c`: the net code is in new files (`src/c4pc/net.c`,
  `src/c4ix/net.c`, the programs), and those three get a few lines each.

## N1: what was built (2026-11-26)

Everything in N1 above works, in QEMU with KVM: `make test-c4pc-net`
(`src/c4pc/test-c4pc-net.sh`) passes on 4 processors (the guests on their
own) and on 1 (everything on processor 0 in turn).

| file | |
|---|---|
| `src/c4pc/net.c` (new) | the card (NET window) and the switch |
| `src/c4pc/pc/hv.c` | a port for each machine as it starts, VM 0's at `hv_init`; a frame ends a guest's nap (`hv_net_kick`), and a nap asked for with a frame waiting is none |
| `src/c4pc/engine.c`, `c4pc.h` | the window in `dev_rd`/`dev_wr`; INFO's `C4I_NET` |
| `src/c4pc/pc/pc.c`, `par.c` | VM 0's nap ends for a frame (its nap loop, and `par_after` when its other CPUs run) |
| `src/c4ix/net.c` (new) | the kernel's stack |
| `src/c4ix/sys.c`, `vfs.c`, `sched.c`, `boot.c`, `c4ix.h` | syscalls 258-262, VN_SOCK, the poll each ms in the trap, `net_init` |
| `src/c4ix/include/c4ix_net.h`, `user/ping.c`, `ifconfig.c`, `nc.c` (new) | userland |
| `src/c4pc/test-c4pc-net.sh`, `qser.py` (new) | the test |

**Measured** (QEMU KVM on helios, 4 processors): ping VM 0 to a guest 2 ms,
guest to guest 1-2 ms, a guest to VM 0 1-4 ms, loopback under 1 ms (C4IX's
clock counts whole milliseconds). Getting there took two fixes:

- The trap took frames in *after* the scheduler had polled its blocked
  tasks, so ping's reply reached its socket just after the look, and the
  idle task's 100 ms nap came first: pings of 100-300 ms. The card is now
  read first.
- VM 0 runs on two CPUs (par.c). When its CPU 0 napped while the other ran,
  `par_after` slept the whole nap without looking at the card: VM 0's first
  answer to a guest came 95 ms late. It now looks every 2 ms.

Not done in N1: TCP, `write()` on a socket (no connected sockets yet), ICMP
"port unreachable", ARP entries never expire (MACs are fixed by machine id,
so nothing goes stale), Machines' Network tab (`net_counts` is there for
INFO), c4pc-host and libjs (N2).

## Trying it (the ISO)

*(the ISO's path, md5 and time are in the report; the steps:)*

1. Boot `c4pc.iso` in VMware or VirtualBox: 2 or more processors, 2 GB of
   memory, no hard disk needed. In GRUB choose **"C4PC hypervisor: this
   CD's C4IX runs the machines (vm)"** (the third entry).
2. On the control C4IX's desktop (dark, with Machines open), open a
   Terminal and make two guests -- C4IX at its text console starts in a
   couple of seconds:

   ```
   vm create a 0 0 c4ix-text
   vm create b 0 0 c4ix-text
   vm up a
   vm up b
   ifconfig            # net0: 10.4.0.1, the control's
   ping a              # 4 replies from 10.4.0.2; ping vm2, ping 10.4.0.3 for b
   ```

   (/vm is in RAM without a disk: the machines are gone at the next boot.
   With an IDE disk, `mkfs hd0`, `mkdir /vm`, `mount hd0 /vm` first, as
   docs/c4pc-hypervisor.md has it.)
3. Talk between the two: in Machines double-click **a** (or `vm attach a`)
   and type `nc -u -l 7000`; Ctrl+Alt+Home comes back. Attach **b**, type
   `ping a`, then `nc -u a 7000` and a line or two: they show on a's
   screen (attach a to see them), and what is typed at a then goes back to
   b. From the control's Terminal, `echo hello | nc -u a 7000` sends one
   line.
4. `vm send a ping -c 3 b` runs a command in a guest without attaching:
   its output is in `vm log` (and Machines' Log tab).
