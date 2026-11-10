#!/usr/bin/env bash
# 一行完成編譯、10 client 並發測試與 tcpdump 擷取：  ./demo.sh [port]
# tcpdump 需要 root，非 root 執行時會用 sudo 詢問一次密碼。
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

check() { # check <說明> <條件指令...>
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

wait_for() { # wait_for <檔案> <字串>，最多等 5 秒
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

start_server() { # start_server <log 檔> <server 參數...>
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
    echo "缺少工具:$missing"
    echo "請先安裝： sudo apt-get install -y build-essential tcpdump"
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo -n"
    echo "tcpdump 需要 root 權限："
    sudo -v || { echo "無法取得 sudo 權限"; exit 1; }
fi

title "1. 第一層 debug 開關：release 版不編入 LOG_DEBUG"
make clean >/dev/null
make -j4 >/dev/null || { echo "編譯失敗"; exit 1; }
mkdir -p "$OUT"
check "release 版建置成功" test -x server -a -x client -a -f lib/libnetproto.so
start_server "$OUT/release.log" -d
./client -p "$PORT" -d >/dev/null 2>"$OUT/release_client.log"
stop_server
check "release 版加 -d 仍沒有 DEBUG 輸出" \
    is_eq "$(cat "$OUT/release.log" "$OUT/release_client.log" | grep -c '\] DEBUG ')" 0
check "release 版提示 -d 無效" has "$OUT/release.log" '-d ignored'
check "release 版 .so 不含 debug 字串" \
    is_eq "$(grep -ac 'rx op=' lib/libnetproto.so)" 0

title "2. debug 版建置與 rpath"
make clean >/dev/null
make -j4 DEBUG=1 >/dev/null || { echo "編譯失敗"; exit 1; }
mkdir -p "$OUT"
readelf -d server | grep -E 'RPATH|RUNPATH|NEEDED.*netproto'
ldd server | grep netproto
check 'server 的 rpath 為 $ORIGIN/lib' has <(readelf -d server) 'RPATH.*\$ORIGIN/lib'
check 'client 的 rpath 為 $ORIGIN/lib' has <(readelf -d client) 'RPATH.*\$ORIGIN/lib'
check "執行時載入專案內的 .so" has <(ldd server) "$PWD/lib/libnetproto.so"

title "3. 啟動 tcpdump"
TCPDUMP=$(command -v tcpdump)
$SUDO sh -c 'echo $$ >"$1"; shift; exec "$@"' sh "$OUT/tcpdump.pid" \
    "$TCPDUMP" -i lo -nn -s 0 -U -w - "tcp port $PORT" \
    >"$OUT/capture.pcap" 2>"$OUT/tcpdump.err" &
CAP_JOB=$!
if ! wait_for "$OUT/tcpdump.err" 'listening on'; then
    cat "$OUT/tcpdump.err"
    echo "tcpdump 啟動失敗"
    exit 1
fi
echo "  擷取中：$OUT/capture.pcap"

title "4. $CLIENTS 個 client 並發（連線上限 $CLIENTS、每條連線停留 ${HOLD_SEC} 秒）"
start_server "$OUT/server.log" -m "$CLIENTS" -t "$IDLE_SEC" -d
SERVER_FIRST=$SERVER_PID

pids=()
for ((i = 1; i <= CLIENTS; i++)); do
    ./client -p "$PORT" -w "$HOLD_SEC" >"$OUT/client_$i.out" 2>"$OUT/client_$i.err" &
    pids+=($!)
done

check "$CLIENTS 條連線同時存活" wait_for "$OUT/server.log" "active=$CLIENTS/$CLIENTS"
workers=$(ps -o pid= --ppid "$SERVER_FIRST" | wc -l)
check "server 有 $CLIENTS 個子程序（實際 $workers）" is_eq "$workers" "$CLIENTS"

title "5. 連線上限：第 $((CLIENTS + 1)) 個 client 應被拒絕"
./client -p "$PORT" >/dev/null 2>"$OUT/client_over.err"
over_rc=$?
check "額外的 client 收到 BUSY（exit=$over_rc）" is_eq "$over_rc" 3
check "server 記錄拒絕連線" has "$OUT/server.log" 'rejected'

ok=0
for pid in "${pids[@]}"; do
    wait "$pid" && ok=$((ok + 1))
done
check "$CLIENTS 個 client 全部成功（實際 $ok）" is_eq "$ok" "$CLIENTS"
distinct=$(cat "$OUT"/client_[0-9]*.out | grep -c '^worker_pid=')
unique=$(cat "$OUT"/client_[0-9]*.out | grep '^worker_pid=' | sort -u | wc -l)
check "每個 client 都拿到系統資訊（實際 $distinct）" is_eq "$distinct" "$CLIENTS"
check "每個 client 由不同子程序服務（實際 $unique）" is_eq "$unique" "$CLIENTS"
echo "  --- client 1 收到的系統資訊 ---"
sed 's/^/  /' "$OUT/client_1.out"

title "6. 子程序回收"
sleep 0.5
zombies=$(ps -o stat= --ppid "$SERVER_FIRST" | grep -c '^Z')
left=$(ps -o pid= --ppid "$SERVER_FIRST" | wc -l)
check "沒有殭屍程序（實際 $zombies）" is_eq "$zombies" 0
check "子程序已全部結束（剩 $left）" is_eq "$left" 0

title "7. idle timeout：${IDLE_SEC} 秒沒有請求就斷線"
./client -p "$PORT" -w "$IDLE_CLIENT_SEC" >/dev/null 2>"$OUT/client_idle.err"
idle_rc=$?
check "idle client 收到 server 錯誤（exit=$idle_rc）" is_eq "$idle_rc" 4
check "server 記錄 idle timeout" has "$OUT/server.log" 'closing connection: idle timeout'
check "第二層 debug：有 -d 時輸出 DEBUG" is_ge "$(grep -c '\] DEBUG ' "$OUT/server.log")" 1

title "8. SO_REUSEADDR：同一 port 立刻重啟"
stop_server
grep ' shutdown ' "$OUT/server.log" | sed 's/^/  /'
check "尖峰並發數達 $CLIENTS" has "$OUT/server.log" "peak=$CLIENTS\$"
start_server "$OUT/restart.log"
check "重啟後 bind 成功" has "$OUT/restart.log" 'listening on'
./client -p "$PORT" -q ping 2>/dev/null | sed 's/^/  ping: /'
check "第二層 debug：沒有 -d 時無 DEBUG 輸出" \
    is_eq "$(grep -c '\] DEBUG ' "$OUT/restart.log")" 0
stop_server

title "9. tcpdump 結果"
stop_capture
FILTER='tcp[((tcp[12] & 0xf0) >> 2):2] = 0x4e53'
frames=$(tcpdump -nn -r "$OUT/capture.pcap" "$FILTER" 2>/dev/null | wc -l)
expected=$((CLIENTS * 4))
check "擷取到 Magic=0x4E53 的封包至少 $expected 個（實際 $frames）" is_ge "$frames" "$expected"
echo "  --- 同一條連線的請求與回應 ---"
first_port=$(tcpdump -nn -r "$OUT/capture.pcap" -c 1 "$FILTER" 2>/dev/null |
    sed -E 's/.*127\.0\.0\.1\.([0-9]+) > .*/\1/')
tcpdump -nn -r "$OUT/capture.pcap" -X -c 2 "tcp port $first_port and $FILTER" 2>/dev/null |
    sed 's/^/  /'

# 每組對照用新的 port，避免前一輪的 TIME_WAIT 影響結果
PORT=$((20000 + RANDOM % 20000))
next_port() { PORT=$((PORT + 1)); }
zombie_count() { ps -o stat= --ppid "$SERVER_PID" | grep -c '^Z'; }
worker_count() { ps -o pid= --ppid "$SERVER_PID" | wc -l; }

title "10. 對照 A：子程序回收（SIGCHLD + waitpid）"
reap_trial() { # reap_trial <log 名> [server 參數...]
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
wait_clients() { # 只等 client，不等 server 與 tcpdump
    local job
    for job in $(jobs -p); do
        [ "$job" = "$SERVER_PID" ] || wait "$job"
    done
}
reap_trial reap_on
with=$REAP_ZOMBIES
reap_trial reap_off -X reap
without=$REAP_ZOMBIES
echo "  有機制：殭屍程序 $with 個；無機制：殭屍程序 $without 個"
check "有回收機制時沒有殭屍程序" is_eq "$with" 0
check "無回收機制時留下 5 個殭屍程序" is_eq "$without" 5

title "11. 對照 B：idle timeout（select）"
idle_trial() { # idle_trial <log 名> [server 參數...]
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
echo "  閒置 3 秒後的子程序：有機制 $with 個（client exit=$with_rc）；無機制 $without 個（client exit=$without_rc）"
check "有 timeout 時閒置連線被釋放" is_eq "$with" 0
check "有 timeout 時 client 收到錯誤" is_eq "$with_rc" 4
check "無 timeout 時閒置連線仍佔用子程序" is_eq "$without" 1

title "12. 對照 C：連線上限（-m 5，同時 8 個 client）"
limit_trial() { # limit_trial <log 名> [server 參數...]
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
echo "  有機制：${with#* shutdown }"
echo "  無機制：${without#* shutdown }"
check "有上限時拒絕 3 條連線且尖峰為 5" grep -q 'rejected=3 .*peak=5$' <<<"$with"
check "無上限時 8 條連線全部進入" grep -q 'rejected=0 .*peak=8$' <<<"$without"

title "13. 對照 D：SO_REUSEADDR（server 主動關閉後立刻重啟）"
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
echo "  重啟前 port $PORT 的 TIME_WAIT 連線：$(ss -tan state time-wait "( sport = :$PORT )" | tail -n +2 | wc -l) 條"
./server -p "$PORT" -X reuseaddr 2>"$OUT/cmp_reuse_off2.log" &
SERVER_PID=$!
sleep 0.5
kill -0 "$SERVER_PID" 2>/dev/null
without_rc=$?
stop_server
grep 'bind' "$OUT/cmp_reuse_off2.log" | sed 's/^/  無機制：/'
check "有 SO_REUSEADDR 時重啟成功" is_eq "$with_rc" 0
check "無 SO_REUSEADDR 時 bind 失敗" has "$OUT/cmp_reuse_off2.log" 'Address already in use'

title "14. 對照 E：CRC 驗證（送出被竄改的封包）"
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
echo "  有機制：client exit=$with_rc，server 記錄：$(grep -o 'closing connection.*' "$OUT/cmp_crc_on.log")"
echo "  無機制：client exit=$without_rc，server 接受並回傳：$(cat "$OUT/cmp_crc_off.out")"
check "有 CRC 時拒絕被竄改的封包" has "$OUT/cmp_crc_on.log" 'CRC mismatch'
check "無 CRC 時被竄改的資料被接受" has "$OUT/cmp_crc_off.out" '^Hello from'

printf '\n結果：%d 通過，%d 失敗。輸出在 %s/\n' "$PASS" "$FAIL" "$OUT"
[ "$FAIL" -eq 0 ]
