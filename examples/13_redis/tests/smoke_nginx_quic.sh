#!/usr/bin/env bash
# HTTP/3 over libdpdknet UDP, using a private AF_PACKET/veth laboratory.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $EUID == 0 ]] || { echo 'Run using sudo'; exit 1; }
ROOT=$PWD
MODE=${NGINX_TEST_MODE:-single-poll}
quic_extra=
case "$MODE" in
 single-poll) master=off; workers=1; event_backend='use poll;' ;;
 single-default) master=off; workers=1; event_backend='' ;;
 master-poll) master=on; workers=2; event_backend='use poll;' ;;
 master-default) master=on; workers=2; event_backend='' ;;
 master-default-reuseport) master=on; workers=2; event_backend=''; quic_extra=reuseport ;;
 *) echo "Unknown NGINX_TEST_MODE=$MODE"; exit 2 ;;
esac
B=${QUIC_LAB_ROOT:-$ROOT/build/quic-lab}
PY="$B/python/bin/python3.11"
NGINX="$B/nginx/sbin/nginx"
for tool in ip ethtool timeout; do command -v "$tool" >/dev/null; done
[[ -x $PY && -x $NGINX ]]
cc -shared -fPIC tests/nginx_quic_audit.c -ldl -o "$B/managed_audit.so"
RUN="$B/run-${MODE}-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/html" "$RUN/logs"; chmod 755 "$RUN" "$RUN/html"
suffix=$$; ns="dq$suffix"; left="dq${suffix}a"; right="dq${suffix}b"
pid=''; capture=''; ns_owned=0; link_owned=0
cleanup() {
 if [[ -n $pid ]]; then
  kill -TERM -- "-$pid" 2>/dev/null || true
  for i in $(seq 1 30); do kill -0 -- "-$pid" 2>/dev/null || break; sleep .1; done
  if kill -0 -- "-$pid" 2>/dev/null; then
   echo 'NOTE: test process group did not exit within 3s after SIGTERM; force-stopping own group'
   kill -KILL -- "-$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
 fi
 if [[ -n $capture ]]; then kill "$capture" 2>/dev/null || true; wait "$capture" 2>/dev/null || true; fi
 if [[ $link_owned == 1 ]]; then ip link del "$left" 2>/dev/null || true; fi
 if [[ $ns_owned == 1 ]]; then ip netns del "$ns" 2>/dev/null || true; fi
 echo "Artifacts: $RUN"
}
trap cleanup EXIT
ip netns add "$ns"; ns_owned=1
ip link add "$left" type veth peer name "$right"; link_owned=1
ip link set "$right" netns "$ns"; ip link set "$left" up
ip netns exec "$ns" ip link set lo up
ip netns exec "$ns" ip addr add 192.0.2.1/24 dev "$right"
ip netns exec "$ns" ip link set "$right" up
ethtool -K "$left" tx off rx off tso off gso off gro off >"$RUN/offload.log" 2>&1
ip netns exec "$ns" ethtool -K "$right" tx off rx off tso off gso off gro off >>"$RUN/offload.log" 2>&1
cpu=$(awk '/Cpus_allowed_list/{split($2,a,/[-,]/);print a[1]}' /proc/self/status)
export DPDKNET_IP=192.0.2.2
export DPDKNET_EAL_ARGS="-l $cpu -n 1 --no-huge --no-pci --file-prefix=dq$suffix --vdev=net_af_packet0,iface=$left"
export LD_LIBRARY_PATH="$ROOT/../../x86_64-native-linux-gcc/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
printf 'dpdk-nginx-http3\n' > "$RUN/html/hello.txt"
python3 -c 'import sys; open(sys.argv[1],"wb").write(bytes(range(256))*1024)' "$RUN/html/payload.bin"
"$B/openssl/bin/openssl" req -config /dev/null -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=quic-lab -keyout "$RUN/key.pem" -out "$RUN/cert.pem" >"$RUN/certificate.log" 2>&1
cat > "$RUN/nginx.conf" <<CONF
daemon off;
master_process $master;
worker_processes $workers;
pid $RUN/nginx.pid;
error_log $RUN/logs/error.log debug;
events { worker_connections 128; ${event_backend} }
http {
    log_format quic '\$remote_addr \$status \$server_protocol http3=\$http3 bytes=\$body_bytes_sent';
    access_log $RUN/logs/access.log quic;
    sendfile off;
    server {
        listen 192.0.2.2:18443 quic ${quic_extra};
        listen 192.0.2.2:18080;
        server_name quic-lab;
        ssl_certificate $RUN/cert.pem;
        ssl_certificate_key $RUN/key.pem;
        ssl_protocols TLSv1.3;
        quic_gso off;
        root $RUN/html;
    }
}
CONF
"$NGINX" -V > "$RUN/versions.log" 2>&1
sha256sum "$ROOT/build/libdpdknet.so" >> "$RUN/versions.log"
if command -v tcpdump >/dev/null; then
 tcpdump -U -n -i "$left" -w "$RUN/wire.pcap" 'arp or udp port 18443' >"$RUN/tcpdump.log" 2>&1 & capture=$!
fi
setsid env LD_PRELOAD="$B/managed_audit.so:$ROOT/build/libdpdknet.so" "$NGINX" -p "$RUN/" -c "$RUN/nginx.conf" >"$RUN/server.log" 2>&1 & pid=$!
sleep 2
ps -eo pid,ppid,pgid,args | awk -v group="$pid" 'NR==1 || $3==group' > "$RUN/processes.log"
echo "MODE=$MODE master=$master workers=$workers"
set +e
kill -0 "$pid" 2>/dev/null
startup_rc=$?
grep 'DPDK_QUIC_MANAGED_BIND.*managed_udp=1' "$RUN/server.log"
audit_rc=$?
grep libdpdknet /proc/"$pid"/maps > "$RUN/preload-maps.log" 2>/dev/null
ip netns exec "$ns" timeout 6 python3 -c 'import urllib.request; r=urllib.request.urlopen("http://192.0.2.2:18080/hello.txt",timeout=3); assert r.status==200; assert r.read()==b"dpdk-nginx-http3\n"; print("PASS Nginx HTTP/1 over DPDK TCP")' > "$RUN/tcp-client.log" 2>&1
tcp_rc=$?
# Always attempt real QUIC, even if startup or TCP has already failed.
ip netns exec "$ns" timeout 25 "$PY" tests/nginx_quic_client.py 192.0.2.2 18443 > "$RUN/client.log" 2>&1
quic_rc=$?
count=$(grep -c 'HTTP/3.0 http3=h3' "$RUN/logs/access.log" 2>/dev/null)
count=${count:-0}
printf 'mode=%s startup_rc=%s audit_rc=%s tcp_rc=%s quic_rc=%s http3_access_count=%s\n' "$MODE" "$startup_rc" "$audit_rc" "$tcp_rc" "$quic_rc" "$count" | tee "$RUN/result.txt"
cat "$RUN/tcp-client.log" "$RUN/client.log"
grep -E 'using the .* event method|\[(emerg|alert|crit)\]' "$RUN/logs/error.log" | head -20
if [[ $startup_rc == 0 && $audit_rc == 0 && $tcp_rc == 0 && $quic_rc == 0 && $count == 6 ]]; then
 echo "PASS: $MODE Nginx HTTP/TCP and HTTP/3 over managed DPDK sockets"
 exit 0
fi
echo "FAIL: $MODE (see result.txt and logs)"
exit 1
