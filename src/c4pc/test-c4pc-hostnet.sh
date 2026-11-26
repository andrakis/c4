#!/bin/bash
# test-c4pc-hostnet.sh -- the network card outside the hypervisor
# (docs/c4pc-network.md, N2; make test-c4pc-hostnet): machines that are
# processes, their cable a pair of UDP sockets on this host's loopback, no
# QEMU. C4IX, this tree's kernel and disk (.c4pc/kernel/c4ix32.c4r,
# .c4pc/disk.tar), on:
#   - two c4pc-hosts (--net udp:..., ids 1 and 2: 10.4.0.2 and .3). b serves
#     (httpd -d -n 1 /, then nc -l 7300); a pings b by its name (vm2), fetches
#     b's /bin/ over HTTP, and sends b a line over TCP
#   - cli.js (libjs's card, net.js) and a c4pc-host: the same, the other way
#     round -- the same frames, so one of each is one network
# Needs c4pc-host, .c4pc/kernel/c4ix32.c4r and .c4pc/disk.tar (make c4pc-host c4pc.iso), and node.
set -u
cd "$(dirname "$0")/../.."
W=.c4pc/hostnet; rm -rf $W; mkdir -p $W/disk
fail=0
ok () { echo "  ok    $*"; }
bad () { echo "  FAIL  $*"; fail=1; }
for f in c4pc-host .c4pc/kernel/c4ix32.c4r .c4pc/disk.tar; do
  [ -f $f ] || { echo "test-c4pc-hostnet: $f is not built (make c4pc-host c4pc.iso)"; exit 1; }
done
tar -xf .c4pc/disk.tar -C $W/disk
K=.c4pc/kernel/c4ix32.c4r
# b: the server, until a's line has come (or 30 s idle); what is left of
# its stdin nc sends to a, so nothing is
printf 'httpd -d -n 1 /\necho b-ready\nnc -l -w 30 7300\n' > $W/b.in
# a: after b has had time to start
printf 'ping -c 3 vm2\necho GET /bin/ HTTP/1.0 | nc vm2 80\necho a line over the cable | nc vm2 7300\necho a-done\n' > $W/a.in
# pair TAG WHAT A-RUNNER B-RUNNER: b started, then a; both waited for
pair () {
  local t=$1 ra=$3 rb=$4
  echo "  ---   $2"
  timeout 60 $rb --net udp:7402:7401 --net-id 2 -m 64 -d $W/disk $K < $W/b.in > $W/$t-b.out 2> $W/$t-b.err &
  local pb=$!
  sleep 4
  timeout 50 $ra --net udp:7401:7402 --net-id 1 -m 64 -d $W/disk $K < $W/a.in > $W/$t-a.out 2> $W/$t-a.err
  local sa=$?
  wait $pb; local sb=$?
  tr -d '\r' < $W/$t-a.out > $W/$t-a.lf; tr -d '\r' < $W/$t-b.out > $W/$t-b.lf
  [ $sa = 0 ] && ok "a ends with status 0" || bad "a: status $sa ($(tail -2 $W/$t-a.err | tr '\n' ' '))"
  [ $sb = 0 ] && ok "b ends with status 0" || bad "b: status $sb ($(tail -2 $W/$t-b.err | tr '\n' ' '))"
  grep -qa "net0: c4net, MAC 52:54:00:c4:00:01, 10.4.0.2/24" $W/$t-a.lf && ok "a's card: 10.4.0.2" || bad "a's card not found"
  grep -qa "3 packets transmitted, 3 received, 0% packet loss" $W/$t-a.lf && ok "a pings b (vm2): 3 replies" || bad "a's pings unanswered"
  grep -qa "64 bytes from 10.4.0.3: icmp_seq=1" $W/$t-a.lf && ok "the replies came from 10.4.0.3" || bad "no reply from 10.4.0.3"
  grep -qa "^HTTP/1.0 200 OK" $W/$t-a.lf && ok "httpd: b's /bin/ fetched" || bad "httpd: no answer"
  grep -qaF '<li><a href="sh">sh</a></li>' $W/$t-a.lf && ok "httpd: its list has sh" || bad "httpd: no list"
  grep -qa "10.4.0.2 GET /bin/ 200" $W/$t-b.lf && ok "httpd: b served a" || bad "httpd: b did not say so"
  grep -qa "a line over the cable" $W/$t-b.lf && ok "TCP: a's line at b" || bad "TCP: a's line never came"
  grep -qa "^a-done" $W/$t-a.lf && grep -qa "^b-ready" $W/$t-b.lf && ok "both shells went on to the end" || bad "a shell did not finish"
  grep -aE "rtt min" $W/$t-a.lf | sed 's/^/        /'
}
pair hh "two c4pc-hosts" ./c4pc-host ./c4pc-host
if command -v node > /dev/null; then
  pair jh "cli.js (a) and c4pc-host (b)" "node libjs/cli.js" ./c4pc-host
else echo "  skip  cli.js and c4pc-host (no node)"; fi
[ $fail = 0 ] && echo "test-c4pc-hostnet: passed" || echo "test-c4pc-hostnet: FAILED (the machines' output: $W)"
exit $fail
