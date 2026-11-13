#!/usr/bin/env bash
# One command builds, runs 10 concurrent clients and captures with tcpdump:  ./demo.sh [port]
# tcpdump needs root; when run as a normal user, sudo asks for the password once.
set -u
cd "$(dirname "$(readlink -f "$0")")" || exit 1

PORT=${1:-19090}
OUT=demo_out
CLIENTS=10
HOLD_SEC=3
IDLE_SEC=5
IDLE_CLIENT_SEC=7

PASS=0
FAIL=0
SERVER_PID=
CAP_JOB=
SUDO=

title() { printf '\n=== %s ===\n' "$1"; }

check() { # check <description> <condition command...>
    local desc=$1
    shift
    if "$@"; then
        printf '  [PASS] %s\n' "$desc"
        PASS=$((PASS + 1))
    else
        printf '  [FAIL] %s\n' "$desc"
        FAIL=$((FAIL + 1))
    fi
}

wait_for() { # wait_for <file> <string>, waits up to 5 seconds
    local i
    for ((i = 0; i < 50; i++)); do
        grep -q -- "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

is_eq() { [ "$1" -eq "$2" ]; }
is_ge() { [ "$1" -ge "$2" ]; }
has() { grep -q -- "$2" "$1"; }

start_server() { # start_server <log file> <server args...>
    local log=$1
    shift
    ./server -p "$PORT" "$@" 2>"$log" &
    SERVER_PID=$!
    wait_for "$log" 'listening on'
}

stop_server() {
    [ -n "$SERVER_PID" ] || return 0
    kill -TERM "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=
}

stop_capture() {
    [ -n "$CAP_JOB" ] || return 0
    sleep 0.3
    $SUDO kill -INT "$(cat "$OUT/tcpdump.pid")" 2>/dev/null
    wait "$CAP_JOB" 2>/dev/null
    CAP_JOB=
}

cleanup() {
    stop_server
    stop_capture
}
trap cleanup EXIT
trap 'exit 130' INT TERM

missing=
for tool in gcc make tcpdump; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    echo "Missing tools:$missing"
    echo "Install first: sudo apt-get install -y build-essential tcpdump"
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo -n"
    echo "tcpdump needs root privileges:"
    sudo -v || { echo "Cannot obtain sudo privileges"; exit 1; }
fi

title "1. Debug level 1 (compile time): release build has no LOG_DEBUG"
make clean >/dev/null
make -j4 >/dev/null || { echo "Build failed"; exit 1; }
mkdir -p "$OUT"
check "release build succeeds" test -x server -a -x client -a -f lib/libnetproto.so
start_server "$OUT/release.log" -d
./client -p "$PORT" -d >/dev/null 2>"$OUT/release_client.log"
stop_server
check "release build prints no DEBUG even with -d" \
    is_eq "$(cat "$OUT/release.log" "$OUT/release_client.log" | grep -c '\] DEBUG ')" 0
check "release build warns that -d is ignored" has "$OUT/release.log" '-d ignored'
check "release .so contains no debug strings" \
    is_eq "$(grep -ac 'rx op=' lib/libnetproto.so)" 0

title "2. Debug build, shared library and rpath"
make clean >/dev/null
make -j4 DEBUG=1 >/dev/null || { echo "Build failed"; exit 1; }
mkdir -p "$OUT"
readelf -d server | grep -E 'RPATH|RUNPATH|NEEDED.*netproto'
ldd server | grep netproto
check 'server rpath is $ORIGIN/lib' has <(readelf -d server) 'RPATH.*\$ORIGIN/lib'
check 'client rpath is $ORIGIN/lib' has <(readelf -d client) 'RPATH.*\$ORIGIN/lib'
check "the project's own .so is loaded at run time" has <(ldd server | sed 's#/\./#/#g') "$PWD/lib/libnetproto.so"

title "3. Start tcpdump"
TCPDUMP=$(command -v tcpdump)
$SUDO sh -c 'echo $$ >"$1"; shift; exec "$@"' sh "$OUT/tcpdump.pid" \
    "$TCPDUMP" -i lo -nn -s 0 -U -w - "tcp port $PORT" \
    >"$OUT/capture.pcap" 2>"$OUT/tcpdump.err" &
CAP_JOB=$!
if ! wait_for "$OUT/tcpdump.err" 'listening on'; then
    cat "$OUT/tcpdump.err"
    echo "tcpdump failed to start"
    exit 1
fi
echo "  capturing to: $OUT/capture.pcap"

title "4. $CLIENTS concurrent clients (limit $CLIENTS, each connection held ${HOLD_SEC} s)"
start_server "$OUT/server.log" -m "$CLIENTS" -t "$IDLE_SEC" -d
SERVER_FIRST=$SERVER_PID

pids=()
for ((i = 1; i <= CLIENTS; i++)); do
    ./client -p "$PORT" -w "$HOLD_SEC" >"$OUT/client_$i.out" 2>"$OUT/client_$i.err" &
    pids+=($!)
done

check "$CLIENTS connections alive at the same time" wait_for "$OUT/server.log" "active=$CLIENTS/$CLIENTS"
workers=$(ps -o pid= --ppid "$SERVER_FIRST" | wc -l)
check "server has $CLIENTS child processes (actual $workers)" is_eq "$workers" "$CLIENTS"

title "5. Connection limit: client $((CLIENTS + 1)) must be rejected"
./client -p "$PORT" >/dev/null 2>"$OUT/client_over.err"
over_rc=$?
check "extra client receives BUSY (exit=$over_rc)" is_eq "$over_rc" 3
check "server logs the rejection" has "$OUT/server.log" 'rejected'

ok=0
for pid in "${pids[@]}"; do
    wait "$pid" && ok=$((ok + 1))
done
check "all $CLIENTS clients succeed (actual $ok)" is_eq "$ok" "$CLIENTS"
distinct=$(cat "$OUT"/client_[0-9]*.out | grep -c '^worker_pid=')
unique=$(cat "$OUT"/client_[0-9]*.out | grep '^worker_pid=' | sort -u | wc -l)
check "every client receives system info (actual $distinct)" is_eq "$distinct" "$CLIENTS"
check "each client served by a different child (actual $unique)" is_eq "$unique" "$CLIENTS"
echo "  --- system info received by client 1 ---"
sed 's/^/  /' "$OUT/client_1.out"

title "6. Child process reaping"
sleep 0.5
zombies=$(ps -o stat= --ppid "$SERVER_FIRST" | grep -c '^Z')
left=$(ps -o pid= --ppid "$SERVER_FIRST" | wc -l)
check "no zombie processes (actual $zombies)" is_eq "$zombies" 0
check "all children have exited (left $left)" is_eq "$left" 0

title "7. Idle timeout: disconnect after ${IDLE_SEC} s without a request"
./client -p "$PORT" -w "$IDLE_CLIENT_SEC" >/dev/null 2>"$OUT/client_idle.err"
idle_rc=$?
check "idle client receives a server error (exit=$idle_rc)" is_eq "$idle_rc" 4
check "server logs idle timeout" has "$OUT/server.log" 'closing connection: idle timeout'
check "debug level 2 (run time): DEBUG output with -d" is_ge "$(grep -c '\] DEBUG ' "$OUT/server.log")" 1

title "8. SO_REUSEADDR: restart on the same port immediately"
stop_server
grep ' shutdown ' "$OUT/server.log" | sed 's/^/  /'
check "peak concurrency reached $CLIENTS" has "$OUT/server.log" "peak=$CLIENTS\$"
start_server "$OUT/restart.log"
check "bind succeeds after restart" has "$OUT/restart.log" 'listening on'
./client -p "$PORT" -q ping 2>/dev/null | sed 's/^/  ping: /'
check "debug level 2 (run time): no DEBUG output without -d" \
    is_eq "$(grep -c '\] DEBUG ' "$OUT/restart.log")" 0
stop_server

title "9. tcpdump results"
stop_capture
FILTER='tcp[((tcp[12] & 0xf0) >> 2):2] = 0x4e53'
frames=$(tcpdump -nn -r "$OUT/capture.pcap" "$FILTER" 2>/dev/null | wc -l)
expected=$((CLIENTS * 4))
check "captured at least $expected packets with Magic=0x4E53 (actual $frames)" is_ge "$frames" "$expected"
echo "  --- request and reply on the same connection ---"
first_port=$(tcpdump -nn -r "$OUT/capture.pcap" -c 1 "$FILTER" 2>/dev/null |
    sed -E 's/.*127\.0\.0\.1\.([0-9]+) > .*/\1/')
tcpdump -nn -r "$OUT/capture.pcap" -X -c 2 "tcp port $first_port and $FILTER" 2>/dev/null |
    sed 's/^/  /'

# Each comparison uses a fresh port so TIME_WAIT from the previous round cannot affect it
PORT=$((20000 + RANDOM % 20000))
next_port() { PORT=$((PORT + 1)); }
zombie_count() { ps -o stat= --ppid "$SERVER_PID" | grep -c '^Z'; }
worker_count() { ps -o pid= --ppid "$SERVER_PID" | wc -l; }

title "10. Comparison A: child reaping (SIGCHLD + waitpid)"
reap_trial() { # reap_trial <log name> [server args...]
    local log=$OUT/cmp_$1.log i
    shift
    next_port
    start_server "$log" "$@"
    for ((i = 0; i < 5; i++)); do ./client -p "$PORT" >/dev/null 2>&1 & done
    wait_clients
    sleep 0.5
    REAP_ZOMBIES=$(zombie_count)
    stop_server
}
wait_clients() { # wait only for clients, not for the server or tcpdump
    local job
    for job in $(jobs -p); do
        [ "$job" = "$SERVER_PID" ] || wait "$job"
    done
}
reap_trial reap_on
with=$REAP_ZOMBIES
reap_trial reap_off -X reap
without=$REAP_ZOMBIES
echo "  with mechanism: $with zombies; without: $without zombies"
check "with reaping: no zombie processes" is_eq "$with" 0
check "without reaping: 5 zombie processes left" is_eq "$without" 5

title "11. Comparison B: idle timeout (select)"
idle_trial() { # idle_trial <log name> [server args...]
    local log=$OUT/cmp_$1.log
    shift
    next_port
    start_server "$log" -t 2 "$@"
    ./client -p "$PORT" -w 4 >/dev/null 2>&1 &
    local cpid=$!
    sleep 3
    IDLE_WORKERS=$(worker_count)
    wait "$cpid"
    IDLE_RC=$?
    stop_server
}
idle_trial idle_on
with=$IDLE_WORKERS with_rc=$IDLE_RC
idle_trial idle_off -X idle
without=$IDLE_WORKERS without_rc=$IDLE_RC
echo "  children after 3 s idle: with mechanism $with (client exit=$with_rc); without $without (client exit=$without_rc)"
check "with timeout: idle connection released" is_eq "$with" 0
check "with timeout: client receives an error" is_eq "$with_rc" 4
check "without timeout: idle connection still holds a child" is_eq "$without" 1

title "12. Comparison C: connection limit (-m 5, 8 clients at once)"
limit_trial() { # limit_trial <log name> [server args...]
    local log=$OUT/cmp_$1.log i
    shift
    next_port
    start_server "$log" -m 5 "$@"
    for ((i = 0; i < 8; i++)); do ./client -p "$PORT" -w 2 >/dev/null 2>&1 & done
    wait_clients
    stop_server
    LIMIT_SUMMARY=$(grep ' shutdown ' "$log")
}
limit_trial limit_on
with=$LIMIT_SUMMARY
limit_trial limit_off -X limit
without=$LIMIT_SUMMARY
echo "  with mechanism: ${with#* shutdown }"
echo "  without:        ${without#* shutdown }"
check "with limit: 3 connections rejected, peak 5" grep -q 'rejected=3 .*peak=5$' <<<"$with"
check "without limit: all 8 connections admitted" grep -q 'rejected=0 .*peak=8$' <<<"$without"

title "13. Comparison D: SO_REUSEADDR (restart right after the server closed connections)"
next_port
start_server "$OUT/cmp_reuse_on1.log"
./client -p "$PORT" -q ping >/dev/null 2>&1
stop_server
./server -p "$PORT" 2>"$OUT/cmp_reuse_on2.log" &
SERVER_PID=$!
sleep 0.5
kill -0 "$SERVER_PID" 2>/dev/null
with_rc=$?
stop_server

next_port
start_server "$OUT/cmp_reuse_off1.log" -X reuseaddr
./client -p "$PORT" -q ping >/dev/null 2>&1
stop_server
echo "  TIME_WAIT connections on port $PORT before restart: $(ss -tan state time-wait "( sport = :$PORT )" | tail -n +2 | wc -l)"
./server -p "$PORT" -X reuseaddr 2>"$OUT/cmp_reuse_off2.log" &
SERVER_PID=$!
sleep 0.5
kill -0 "$SERVER_PID" 2>/dev/null
without_rc=$?
stop_server
grep 'bind' "$OUT/cmp_reuse_off2.log" | sed 's/^/  without: /'
check "with SO_REUSEADDR: restart succeeds" is_eq "$with_rc" 0
check "without SO_REUSEADDR: bind fails" has "$OUT/cmp_reuse_off2.log" 'Address already in use'

title "14. Comparison E: CRC check (send a corrupted packet)"
next_port
start_server "$OUT/cmp_crc_on.log"
./client -p "$PORT" -B >"$OUT/cmp_crc_on.out" 2>/dev/null
with_rc=$?
stop_server
next_port
start_server "$OUT/cmp_crc_off.log" -X crc
./client -p "$PORT" -B >"$OUT/cmp_crc_off.out" 2>/dev/null
without_rc=$?
stop_server
echo "  with mechanism: client exit=$with_rc, server log: $(grep -o 'closing connection.*' "$OUT/cmp_crc_on.log")"
echo "  without:        client exit=$without_rc, server accepted and replied: $(cat "$OUT/cmp_crc_off.out")"
check "with CRC: corrupted packet rejected" has "$OUT/cmp_crc_on.log" 'CRC mismatch'
check "without CRC: corrupted data accepted" has "$OUT/cmp_crc_off.out" '^Hello from'

printf '\nResult: %d passed, %d failed. Output in %s/\n' "$PASS" "$FAIL" "$OUT"
[ "$FAIL" -eq 0 ]
