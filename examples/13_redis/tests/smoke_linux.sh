#!/usr/bin/env bash
# Isolated Linux integration: AF_PACKET PMD over a disposable veth pair.
# Does not bind or reconfigure a physical NIC.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $EUID == 0 ]] || { echo 'Run with sudo (network namespaces require root).' >&2; exit 1; }
for command in ip redis-server redis-cli timeout; do command -v "$command" >/dev/null; done
ROOT=$(pwd)
RTE_SDK=${RTE_SDK:-$(cd ../.. && pwd)}
RTE_TARGET=${RTE_TARGET:-x86_64-native-linux-gcc}
export LD_LIBRARY_PATH="$RTE_SDK/$RTE_TARGET/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[[ -f build/libdpdknet.so ]] || { echo 'Build libdpdknet.so first.' >&2; exit 1; }
suffix=$$
ns="dn${suffix}"
left="dn${suffix}a"
right="dn${suffix}b"
pid=''
capture=''
cleanup() {
 if [[ -n $pid ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
 if [[ -n $capture ]]; then kill "$capture" 2>/dev/null || true;wait "$capture" 2>/dev/null || true;fi
 ip link del "$left" 2>/dev/null || true
 ip netns del "$ns" 2>/dev/null || true
 rm -f "$ROOT/build/smoke-${suffix}.rdb"
}
trap cleanup EXIT
ip netns add "$ns"
ip link add "$left" type veth peer name "$right"
ip link set "$right" netns "$ns"
ip link set "$left" up
ip netns exec "$ns" ip link set lo up
ip netns exec "$ns" ip addr add 192.0.2.1/24 dev "$right"
ip netns exec "$ns" ip link set "$right" up
# AF_PACKET sees packets before veth checksum offloads are completed.
if command -v ethtool >/dev/null; then
 ethtool -K "$left" tx off rx off tso off gso off gro off >/dev/null 2>&1
 ip netns exec "$ns" ethtool -K "$right" tx off rx off tso off gso off gro off >/dev/null 2>&1
fi
cpu=$(awk '/Cpus_allowed_list/{split($2,a,/[-,]/);print a[1]}' /proc/self/status)
export DPDKNET_IP=192.0.2.2
export DPDKNET_EAL_ARGS="-l $cpu -n 1 --no-huge --no-pci --file-prefix=dn${suffix} --vdev=net_af_packet0,iface=$left"
mkdir -p build
if command -v tcpdump >/dev/null; then
 tcpdump -l -n -i "$left" -vv >build/wire-smoke.log 2>&1 &
 capture=$!
fi
start_stack() {
 env LD_PRELOAD="$ROOT/build/libdpdknet.so" "$@" >"build/$1-smoke.log" 2>&1 &
 pid=$!
}
stop_stack() { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; pid=''; }
start_stack redis-server --bind "$DPDKNET_IP" --port 16379 --save '' --appendonly no --daemonize no --dir "$ROOT/build" --dbfilename "smoke-${suffix}.rdb"
ok=0
for i in $(seq 1 30); do
 if ip netns exec "$ns" timeout 1 redis-cli -h "$DPDKNET_IP" -p 16379 ping 2>/dev/null | grep -q PONG; then ok=1;break;fi
 kill -0 "$pid" 2>/dev/null || break
 sleep 0.2
done
[[ $ok == 1 ]] || { cat build/redis-server-smoke.log;echo 'Redis startup/PING failed' >&2;exit 1; }
[[ $(ip netns exec "$ns" timeout 5 redis-cli -h "$DPDKNET_IP" -p 16379 set dpdknet:test hello) == OK ]]
[[ $(ip netns exec "$ns" timeout 5 redis-cli -h "$DPDKNET_IP" -p 16379 get dpdknet:test) == hello ]]
echo 'PASS: Redis PING / SET / GET through DPDK stack'
ip netns exec "$ns" timeout 30 python3 tests/redis_stream.py "$DPDKNET_IP" 16379
stop_stack
# These public API tests assert that nisdgram(fd) is true in the loaded so.
cc -std=gnu11 -O1 -g tests/test_udp_api.c -ldl -o build/test_udp_api
env LD_PRELOAD="$ROOT/build/libdpdknet.so" build/test_udp_api server "$DPDKNET_IP" >build/udp-server-smoke.log 2>&1 &
pid=$!
ip netns exec "$ns" timeout 20 python3 tests/udp_peer.py client "$DPDKNET_IP"
wait "$pid";pid=''
timeout 25 ip netns exec "$ns" python3 tests/udp_peer.py server 192.0.2.1 >build/udp-kernel-peer.log 2>&1 &
pid=$!;sleep .3
timeout 20 env LD_PRELOAD="$ROOT/build/libdpdknet.so" build/test_udp_api client 192.0.2.1 >build/udp-client-smoke.log 2>&1
wait "$pid";pid=''
echo 'PASS: UDP preload API server and client (asserted managed UDP descriptors)'
if command -v iperf3 >/dev/null; then
 for mode in normal reverse parallel udp udp-reverse udp-parallel; do
  start_stack iperf3 -4 -s -1 -B "$DPDKNET_IP" -p 15201
  sleep 1
  extra=(-P 1);[[ $mode != reverse ]] || extra=(-R);[[ $mode != parallel ]] || extra=(-P 4)
  [[ $mode != udp ]] || extra=(-u -l 1200 -b 1M)
  [[ $mode != udp-reverse ]] || extra=(-u -l 1200 -b 1M -R)
  [[ $mode != udp-parallel ]] || extra=(-u -l 1200 -b 1M -P 4)
  ip netns exec "$ns" timeout 20 iperf3 -4 -c "$DPDKNET_IP" -p 15201 -t 2 "${extra[@]}" >"build/iperf3-$mode-smoke.log" 2>&1
  wait "$pid";pid=''
  echo "PASS: iperf3 stack server $mode"
 done
 # Same server PID must survive every test and rebuild its listeners. This
 # covers immediate rebinding and TCP/UDP protocol switches missed by -1 tests.
 env LD_PRELOAD="$ROOT/build/libdpdknet.so" iperf3 -4 -s -B "$DPDKNET_IP" -p 15201 >build/iperf3-persistent-server.log 2>&1 &
 pid=$!
 sleep 1
 for mode in normal reverse parallel udp udp-reverse udp-parallel normal-again; do
  extra=(-P 1);[[ $mode != reverse ]] || extra=(-R);[[ $mode != parallel ]] || extra=(-P 4)
  [[ $mode != udp ]] || extra=(-u -l 1200 -b 1M)
  [[ $mode != udp-reverse ]] || extra=(-u -l 1200 -b 1M -R)
  [[ $mode != udp-parallel ]] || extra=(-u -l 1200 -b 1M -P 4)
  kill -0 "$pid"
  ip netns exec "$ns" timeout 20 iperf3 -4 -c "$DPDKNET_IP" -p 15201 -t 2 "${extra[@]}" >"build/iperf3-persistent-$mode.log" 2>&1
  kill -0 "$pid"
  echo "PASS: iperf3 persistent same-PID server $mode"
 done
 if grep -q 'unable to start listener\|Address already in use\|too many errors' build/iperf3-persistent-server.log; then
  cat build/iperf3-persistent-server.log >&2;exit 1
 fi
 stop_stack
 # Stack client (active SYN_SENT), against the namespace kernel server.
 timeout 20 ip netns exec "$ns" iperf3 -4 -s -1 -B 192.0.2.1 -p 15201 >build/iperf3-kernel-server.log 2>&1 &
 pid=$!;sleep 0.3
 timeout 20 env LD_PRELOAD="$ROOT/build/libdpdknet.so" iperf3 -4 -c 192.0.2.1 -p 15201 -t 2 >build/iperf3-client-smoke.log 2>&1
 wait "$pid";pid=''
 echo 'PASS: iperf3 stack client'
 timeout 20 ip netns exec "$ns" iperf3 -4 -s -1 -B 192.0.2.1 -p 15201 >build/iperf3-udp-kernel-server.log 2>&1 &
 pid=$!;sleep .3
 timeout 20 env LD_PRELOAD="$ROOT/build/libdpdknet.so" iperf3 -4 -c 192.0.2.1 -p 15201 -u -l 1200 -b 1M -t 2 >build/iperf3-udp-client-smoke.log 2>&1
 wait "$pid";pid=''
 echo 'PASS: iperf3 UDP stack client' 
else
 echo 'SKIP: iperf3 executable is not installed'
fi
