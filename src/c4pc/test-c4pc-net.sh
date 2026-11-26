#!/bin/bash
# test-c4pc-net.sh -- the network between the hypervisor's machines
# (docs/c4pc-network.md, make test-c4pc-net), in QEMU as the hypervisor's
# own checks run: c4pc.elf --hv with KVM, VM 0 (the control C4IX, at its
# text console on COM1) and two guests, a and b, C4IX at their text
# consoles from the ISO's /vm/images, their consoles on COM1 as "vm1 a:"
# and "vm2 b:" lines. Driven over COM1 (qser.py):
#   - each machine's card (ifconfig): 10.4.0.1, .2 and .3
#   - ping: VM 0 to a, a to b, b to VM 0 (by name), b to itself (loopback),
#     and to an address nobody has (no replies, the ARP unanswered)
#   - UDP: b listening (nc -u -l 7000), VM 0 sends it a line through a
#     pipe, a talks to it (nc -u b 7000, lines typed at a), b answers a
# Needs c4pc.elf, c4pc.iso, .c4pc/kernel/c4ix32.c4r and .c4pc/disk.tar
# (make c4pc.iso), and QEMU with KVM (~/opt/pctools: qemu-system-x86_64).
#   QEMU=path    the emulator   SMP=N   the PC's processors (default 4:
#                the guests on processors of their own)
set -u
cd "$(dirname "$0")/../.."
[ -f ~/opt/pctools/env.sh ] && . ~/opt/pctools/env.sh
Q=${QEMU:-$(command -v qemu-system-x86_64 || echo ~/opt/pctools/bin/qemu-system-x86_64)}
W=.c4pc/net; mkdir -p $W
fail=0
ok () { echo "  ok    $*"; }
bad () { echo "  FAIL  $*"; fail=1; }
for f in c4pc.elf c4pc.iso .c4pc/kernel/c4ix32.c4r .c4pc/disk.tar; do
  [ -f $f ] || { echo "test-c4pc-net: $f is not built (make c4pc.iso)"; exit 1; }
done
if [ ! -x "$Q" ] || [ ! -w /dev/kvm ]; then echo "  skip  the network in QEMU (no qemu-system-x86_64 with KVM)"; exit 0; fi
accel="-accel kvm -cpu host"
rm -f $W/hd.img $W/ser.sock $W/s.txt; truncate -s 512M $W/hd.img
timeout 600 "$Q" $accel -smp ${SMP:-4} -m 2048 -machine pc,vmport=off -display none -no-reboot \
  -serial unix:$W/ser.sock,server=on,wait=off \
  -drive file=$W/hd.img,format=raw,if=ide,index=0 -drive file=c4pc.iso,media=cdrom,if=ide,index=2 \
  -kernel c4pc.elf -append "--hv --mmu --no-store --gui 1024x768 -- --no-top" \
  -initrd ".c4pc/kernel/c4ix32.c4r,.c4pc/disk.tar drive" 2> $W/qemu.err &
q=$!
QSER_T=60 python3 src/c4pc/qser.py $W/ser.sock $W/s.txt "expect:c4ix-sh" "sleep:2" \
  "send:mount cdrom\\n" "sleep:1" "send:mkfs hd0\\n" "sleep:2" "send:mkdir /vm\\n" "send:mount hd0 /vm\\n" "sleep:1" \
  "send:vm create a 0 0 c4ix-text\\n" "expect:vm up a starts it" "send:vm create b 0 0 c4ix-text\\n" "expect:vm up b starts it" \
  "send:vm up a\\n" "expect:vm1 a: c4ix-sh" "send:vm up b\\n" "expect:vm2 b: c4ix-sh" "sleep:1" \
  "send:ifconfig\\n" "expect:RX packets" \
  "send:vm send a ifconfig\\n" "expect:vm1 a:         RX packets" \
  "send:vm send b ifconfig\\n" "expect:vm2 b:         RX packets" \
  "send:ping -c 3 a\\n" "expect:rtt min" \
  "send:vm send a ping -c 3 b\\n" "expect:vm1 a: rtt min" \
  "send:vm send b ping -c 3 control\\n" "expect:vm2 b: rtt min" \
  "send:vm send b ping -c 2 localhost\\n" "expect:vm2 b: rtt min" \
  "send:ping -c 2 -W 500 10.4.0.99\\n" "expect:packet loss" \
  "send:vm send b nc -u -l 7000\\n" "expect:vm2 b: nc: listening" \
  "send:echo hello from control | nc -u b 7000\\n" "expect:vm2 b: hello from control" \
  "send:vm send a nc -u b 7000\\n" "sleep:2" "send:vm send a hello from a\\n" "expect:vm2 b: hello from a" \
  "send:vm send b hi from b\\n" "expect:vm1 a: hi from b" "sleep:1" \
  "send:ifconfig\\n" "expect:sockets open" "sleep:1" \
  "send:shutdown\\n" "sleep:30" > $W/qser.out
st=$?
kill $q 2> /dev/null; wait $q 2> /dev/null
cat $W/qser.out
tr -d '\r' < $W/s.txt > $W/s.lf
L=$W/s.lf
[ $st = 0 ] && ok "every step's answer came" || bad "a step's answer never came (qser: $(cat $W/qser.out))"
has () { grep -qaF -- "$2" $L && ok "$1" || bad "$1 ('$2')"; }
# ... at the start of a line: VM 0's own output, not a guest's
own () { grep -qa -- "^$2" $L && ok "$1" || bad "$1 ('$2')"; }
own "VM 0's card: 10.4.0.1" "        inet 10.4.0.1  netmask 255.255.255.0"
has "a's card: 10.4.0.2" "vm1 a:         inet 10.4.0.2"
has "b's card: 10.4.0.3" "vm2 b:         inet 10.4.0.3"
has "the kernel found the card" "vm1 a: net0: c4net, MAC 52:54:00:c4:00:01, 10.4.0.2/24"
own "VM 0 pings a: 3 replies" "3 packets transmitted, 3 received, 0% packet loss"
has "a pings b: 3 replies" "vm1 a: 3 packets transmitted, 3 received, 0% packet loss"
has "b pings VM 0 by its name: 3 replies" "vm2 b: 3 packets transmitted, 3 received, 0% packet loss"
has "b pings itself (loopback)" "vm2 b: 2 packets transmitted, 2 received, 0% packet loss"
own "nobody at 10.4.0.99: no replies" "2 packets transmitted, 0 received, 100% packet loss"
has "the replies came from b" "vm1 a: 64 bytes from 10.4.0.3: icmp_seq=3 ttl=64"
has "UDP: VM 0 to b through a pipe" "vm2 b: hello from control"
has "UDP: a to b, typed at a" "vm2 b: hello from a"
has "UDP: b answers a" "vm1 a: hi from b"
# every ping's round trip a few ms at most (the guests' naps end for a frame)
worst=$(grep -aoE "time=[0-9]+ ms" $L | grep -oE "[0-9]+" | sort -n | tail -1)
[ -z "$worst" ] || [ "$worst" -le 20 ] && ok "every round trip under 20 ms (the slowest ${worst:-<1} ms)" || bad "a round trip took $worst ms"
grep -aE "rtt min|^vm[12] [ab]: rtt min" $L | sed 's/^/        /'
grep -aqE "panic|c4ix: task [0-9]+ .*(fault|killing)" $L && { bad "a panic or a fault:"; grep -aE "panic|fault|killing" $L | head -5; }
[ $fail = 0 ] && echo "test-c4pc-net: passed" || echo "test-c4pc-net: FAILED (the serial log: $L)"
exit $fail
