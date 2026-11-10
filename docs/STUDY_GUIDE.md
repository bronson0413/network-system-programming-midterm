# 系統導讀：看懂並講解你的 Fork Client–Server

這份文件是給你自己看的。讀完你應該能回答三件事：系統怎麼運作、每段程式為什麼這樣寫、在老師面前怎麼講。

建議閱讀順序：第 1～3 節建立全貌，第 4～6 節對著程式碼看，第 9～11 節在展示前背熟。

---

## 1. 一分鐘版

> 這是用 C 寫的 Linux 系統資訊服務。Server 的父程序只負責接連線，每條連線 `fork()` 一個子程序服務，所以一個 client 出問題不會影響其他 client。兩邊用自訂封包溝通，12 bytes header 含 CRC32 檢查。共用程式包成 `.so`，用 rpath 連結。Server 用 `sigaction` 配 `waitpid` 回收子程序、`select` 處理閒置逾時，也有連線上限和 `SO_REUSEADDR`。實測 500 條並發全部成功，超過 512 條上限時回 BUSY。

只要記得一句話：**父程序管連線，子程序管服務，結束後父程序回收。**

---

## 2. 檔案地圖

| 檔案 | 做什麼 | 先看哪裡 |
| --- | --- | --- |
| `include/netproto.h` | 協定常數、OpCode、錯誤碼、log 巨集 | 開頭的封包格式註解 |
| `src/proto.c` | CRC32、封包收送、帶期限的等待 | `send_frame`、`proto_recv`、`wait_readable` |
| `src/log.c` | 帶時間和 PID 的 log | `log_msg` |
| `src/server.c` | 程序管理、訊號、查系統資訊 | `main`、`on_sigchld`、`run_child`、`serve_client` |
| `src/client.c` | 命令列查詢工具 | `transact`、`main` |
| `Makefile` | 建置 `.so` 和兩個執行檔 | `RPATH` 那行 |
| `demo.sh` | 一行跑完全部驗證 | 每個 `title` 是一個階段 |
| `tests/stress_client.py` | Python 寫的壓力測試 client | `session` |
| `tests/malformed_test.py` | 壞封包與模糊測試 | `main` |

`proto.c` 和 `log.c` 編成 `lib/libnetproto.so`。`server.c` 和 `client.c` 各自編成執行檔，都連結這個函式庫。

---

## 3. 跟著一條連線走一遍

這是最重要的一節，能順著講完，系統就懂了一大半。

1. **啟動。** `main` 解析參數、安裝訊號處理，`create_listener` 依序做 `socket`、`setsockopt(SO_REUSEADDR)`、`bind`、設成非阻塞、`listen`。
2. **等連線。** 主迴圈用 `select` 等 listen socket，最多等 1 秒，時間到就回頭檢查有沒有收到停止訊號。
3. **接受連線。** `accept` 拿到新的 `conn_fd`。
4. **檢查名額。** 先用 `sigprocmask` 擋住訊號，如果 `g_active >= g_max_conn`，回 BUSY 封包後關閉，流程結束。
5. **fork。** 名額足夠就 `fork()`，從這裡分成兩個程序。
   - **父程序：** 把子程序 PID 登記到 `g_children`，`g_active++`，解除訊號遮罩，關閉 `conn_fd`，回到第 2 步。
   - **子程序：** 進入 `run_child`，把訊號處理還原成預設，關閉 `listen_fd`，呼叫 `serve_client`。
6. **服務迴圈。** 子程序重複：`proto_recv` 等請求、處理、`proto_send` 回應。
7. **結束。** 收到 BYE 就回應後關閉；閒置逾時或封包錯誤就回 ERROR 後關閉；client 斷線就直接結束。最後都走到 `_exit()`。
8. **回收。** 子程序結束時核心送 SIGCHLD 給父程序，`on_sigchld` 用 `waitpid(-1, NULL, WNOHANG)` 迴圈回收，並把 `g_active` 減一。

**為什麼父子都要關 fd？** `fork` 後兩邊都持有所有 fd。父程序不關 `conn_fd`，就會耗盡 fd，而且子程序關閉後 client 也收不到斷線。子程序不關 `listen_fd`，就會多持有一份用不到的監聽 socket。

---

## 4. 封包協定

```text
offset  size  欄位
     0     2  Magic    固定 0x4E53（ASCII "NS"）
     2     1  Version  目前為 1
     3     1  OpCode   請求或回應類型
     4     4  Length   payload 長度，不含 header
     8     4  CRC32    涵蓋 header 前 8 bytes + payload
    12     N  Payload  最多 8192 bytes
```

用 tcpdump 實際抓到的查詢請求來讀：

```text
4e53 01 08 00000000 b02464f8
│    │  │  │        └─ CRC32
│    │  │  └─ Length = 0，請求沒有 payload
│    │  └─ OpCode 0x08 = ALL（查詢全部）
│    └─ Version 1
└─ Magic 0x4E53
```

回應是 `4e53 01 88 000000ee 662e4a1b` 加上 238 bytes 的文字：

- `0x88` = `0x08 | 0x80`，代表 ALL 的回應。
- `0xEE` = 238，TCP 長度 250 = 12 + 238，對得上。

**常見問題的答案：**

- **為什麼用 network byte order？** 不同 CPU 的位元組順序可能不同，協定要固定一種。`htons`、`htonl` 轉成網路順序，`ntohs`、`ntohl` 轉回來。
- **為什麼 header 也算 CRC？** 如果只算 payload，OpCode 或 Length 被改掉就偵測不到。
- **為什麼不直接用 struct 傳？** 編譯器可能補 padding，也有對齊問題，所以 `send_frame` 用 `memcpy` 逐欄位寫入。
- **為什麼先檢查 Length 才收資料？** Length 是對方給的，不可信。超過 8192 就直接拒絕，不會照著錯誤的長度繼續讀。

---

## 5. 程式碼導讀：Server

### 5.1 `on_sigchld`：回收子程序

```c
static void on_sigchld(int sig)
{
    int saved_errno = errno;
    pid_t pid;

    (void)sig;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        /* 從 g_children 移除，g_active-- */
    }
    errno = saved_errno;
}
```

- **為什麼是迴圈？** 一般訊號不會排隊。5 個子程序同時結束，可能只收到 1 次 SIGCHLD，只呼叫一次 `waitpid` 就會留下 4 個殭屍。
- **為什麼要 `WNOHANG`？** 沒有 `WNOHANG`，還有子程序活著時 `waitpid` 會卡住，signal handler 也就卡住了整個父程序。
- **`-1` 是什麼？** 等任何一個子程序。
- **為什麼存回 `errno`？** handler 可能打斷正在檢查 `errno` 的主流程，`waitpid` 又會改掉它。
- **為什麼 handler 不寫 log？** `log_msg` 用了 `snprintf` 等函式，它們不是 async-signal-safe。handler 裡只做必要的事。

**什麼是殭屍程序？** 子程序結束後，核心要保留它的結束狀態，等父程序用 `wait` 系列取走。沒取走的就是殭屍：程式不跑了，但還佔著程序表的位置。

### 5.2 `main` 主迴圈

**為什麼 `fork` 前後要擋訊號？**

```c
sigprocmask(SIG_BLOCK, &block_mask, &orig_mask);
if (g_active >= g_max_conn) { /* 拒絕 */ }
pid = fork();
```

`g_active` 同時被主流程和 handler 修改。`g_active++` 不是單一不可分割的動作，如果做到一半被 handler 插入，計數就會錯。子程序還可能在父程序登記 PID 之前就結束，先擋住訊號就能避免 handler 找不到它。

**其他細節：**

- `select` 給 1 秒逾時：停止訊號如果剛好在進入 `select` 前到達，最多延遲 1 秒就會被看見。
- listen socket 設成非阻塞：`select` 說可讀之後，連線仍可能已被對方中止，這時阻塞的 `accept` 會卡住。
- 連線超過上限仍然 `accept`：這樣才能回 BUSY，讓 client 分辨「很忙」和「連不上」。
- `ACCEPT_BACKOFF_USEC`：這是壓力和故障測試後加的，見第 8 節。

### 5.3 `run_child`：子程序初始化

順序很重要：先把訊號處理還原，再解除遮罩，否則子程序可能用到從父程序繼承的 handler。

`SO_SNDTIMEO` 設定傳送期限，client 一直不讀回應時，`send` 不會永遠卡住。

結束用 `_exit` 而不是 `exit`：子程序不需要執行 `atexit` 等清理，也避免重複沖出繼承來的 stdio 緩衝。

### 5.4 `serve_client`：請求迴圈

`rc` 的處理分三種：

| 狀況 | 處理 | 原因 |
| --- | --- | --- |
| `PROTO_ERR_CLOSED` | 直接結束 | client 正常斷線 |
| 其他錯誤（壞封包、逾時） | 回 ERROR 後結束 | 串流已對不齊封包邊界，無法繼續 |
| 未知 OpCode | 回 ERROR 但繼續 | 封包本身完整，只是不認識這個指令 |

`req` 宣告成 `static`：封包緩衝有 8 KB，放靜態區可以少佔堆疊。每個子程序都有自己的記憶體副本，不會互相干擾。

### 5.5 系統資訊

- `gethostname`：主機名
- `uname`：核心名稱、版本、架構
- `sysinfo`：uptime、負載、記憶體、程序數
- `sysconf(_SC_NPROCESSORS_ONLN)`：CPU 數
- `strftime`：格式化時間

`sysinfo` 的 `loads` 是定點數，要除以 65536 才是常見的負載值。

---

## 6. 程式碼導讀：函式庫與 Client

### 6.1 `wait_readable` 和 idle timeout

重點是**整個封包只有一個期限**，不是每次 `recv` 重新計時：

```c
int64_t left = deadline_ms - now_ms();
if (left <= 0)
    return PROTO_ERR_TIMEOUT;
tv.tv_sec = left / 1000;
tv.tv_usec = (left % 1000) * 1000;
```

如果每次讀取都重新計時，攻擊者每 29 秒送 1 byte，就能永遠佔著子程序。用同一個期限，慢速傳送也會被切斷，測試實際在 2.2 秒內斷線。

`select` 被訊號打斷會回傳 `EINTR`，程式用剩餘時間重新等待。用 `CLOCK_MONOTONIC` 是因為系統時間被調整時，它不會跳動。

`FD_SETSIZE` 的檢查也不能少：`select` 只能處理編號小於 1024 的 fd，超過是未定義行為。

### 6.2 `send_frame`

- 整個封包組好後一起 `send`，tcpdump 能看到一個完整封包。
- `MSG_NOSIGNAL`：對方已關閉時回傳錯誤，而不是收到 SIGPIPE 讓程式結束。
- `corrupt` 參數：先算好 CRC 才把 payload 第一個 byte XOR `0x20`，模擬傳輸中被改動。`h` 變成 `H`，所以關掉 CRC 時會看到 `Hello from pid`。

### 6.3 `proto_crc32`

```c
crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
```

這是 IEEE 802.3 CRC-32 的逐位元算法，`0xEDB88320` 是標準多項式的反射形式。`0U - (crc & 1U)` 在最低位是 1 時得到全 1 的遮罩，是 0 時得到 0。Python 的 `zlib.crc32` 能驗證同一個結果，測試程式就是這樣互通的。

### 6.4 兩層 debug log

| 建置 | 執行 | 結果 |
| --- | --- | --- |
| release | 有無 `-d` 都一樣 | 不輸出，字串也不在執行檔裡 |
| debug | 不加 `-d` | 不輸出 |
| debug | 加 `-d` | 輸出 |

第一層是編譯時的 `#ifdef DEBUG`，第二層是執行時的 `-d`。Release 版的巨集用 `if (0)`，編譯器仍檢查參數，最佳化後程式碼和字串都會消失。

### 6.5 `.so` 與 rpath

```make
RPATH := -Wl,-rpath,'$$ORIGIN/lib' -Wl,--disable-new-dtags
```

- `-fPIC`：`.so` 可以載入到任意位址。
- `$ORIGIN`：動態連結器把它換成執行檔所在的目錄。
- `$$`：Makefile 用來表示一個 `$`。單引號防止 shell 把它展開。
- `--disable-new-dtags`：寫入 `RPATH`，而不是新版預設的 `RUNPATH`。

這樣執行檔會找 `./lib/libnetproto.so`，專案搬到別的位置也能跑，不用設 `LD_LIBRARY_PATH`。

用 `readelf -d server | grep RPATH` 可以現場證明。

### 6.6 Client

`transact` 即使送出失敗也會嘗試接收，因為 server 關閉前送的 BUSY 或 ERROR 可能還在接收緩衝區。

結束碼：0 成功、1 連線或協定錯誤、2 參數錯誤、3 server 已滿、4 server 回報錯誤。

收到 BYE 回應後，client 會等 server 先關閉，這樣 TIME_WAIT 固定留在 server 端。

---

## 7. 三組主要對照

Server 的 `-X` 選項可以關閉單一機制，所以差異一定來自那個機制。

### Zombie 回收（`-X reap`）

跑 5 個 client，結果 **0 個 → 5 個**殭屍。

講法：「沒有回收的話，結束的子程序會留在程序表，而且連線計數不會下降，最後新連線全部進不來。」

### Idle timeout（`-X idle`）

期限 2 秒、client 閒置 4 秒，第 3 秒檢查：**0 個 → 1 個**子程序。

講法：「不送資料的 client 如果沒有逾時機制，會無限期佔住一個程序和一個連線名額。」

### SO_REUSEADDR（`-X reuseaddr`）

Server 主動關閉後立刻重啟：**成功 → `Address already in use`**。

講法：「主動關閉連線的一方會進入 TIME_WAIT，在 Linux 上約 60 秒。沒有這個選項，這段時間內 server 重啟會綁定失敗，故障恢復就被延遲了。」

另外兩組是連線上限和 CRC 驗證，可以當補充。

---

## 8. 測試找到的兩個真實問題

老師問「你有測出什麼問題嗎」時，這是最好的答案。

### 問題 1：`listen` 佇列太小

- **現象：** 10～200 條並發建立連線都在 0.05 秒內，500 條卻要約 1.1 秒。
- **推論：** `listen` 佇列是 128，比上限 512 小。佇列滿時，新連線要等 TCP 重送，初次重送約 1 秒。
- **修正：** 佇列改成和上限一樣。
- **驗證：** 500 條的建立時間降到約 0.1 秒。

### 問題 2：fd 用盡時主迴圈空轉

- **現象：** `accept` 回傳 EMFILE 時，連線還在佇列裡，`select` 會立刻再次返回，形成忙等。
- **重現：** 用 `ulimit -n 4` 限制 fd，舊版 3 秒寫出 **1,878,423 行** log。
- **修正：** 這類錯誤後暫停 100 ms。
- **驗證：** 同樣條件只剩 **32 行**。

這兩個問題都是「先量測、提出假設、修改、再驗證」的完整過程，講的時候照這個順序。

---

## 9. 實測數據

中位數取自 3 輪，每條連線 20 個請求，server 上限 512：

| 並發 | 成功 | BUSY | 吞吐量 (req/s) | p50 (ms) | p99 (ms) |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 10 | 0 | 11,990 | 0.73 | 1.18 |
| 50 | 50 | 0 | 12,400 | 3.52 | 5.09 |
| 100 | 100 | 0 | 11,852 | 7.73 | 10.01 |
| 200 | 200 | 0 | 12,429 | 14.70 | 16.85 |
| 500 | 500 | 0 | 12,583 | 36.43 | 42.89 |
| 600 | 512 | 88 | 12,116 | 38.56 | 44.49 |

**怎麼解讀：** 吞吐量大致固定，連線越多，每個請求排隊越久，所以延遲上升。測試 client 是單執行緒 Python，12,000 req/s 可能是 client 的極限，**不要說這是 server 的極限**。

**跨網路測試：** Windows 的 Python client 連到 WSL2 的 server，經過虛擬網卡 `eth0`。30 條連線全部成功，抓到 1,320 個封包，CRC 全部正確。

**稽核：** `gcc -fanalyzer`、AddressSanitizer、UBSan 都沒有回報記憶體錯誤，15 項異常測試和 500 次隨機竄改封包全部通過。

---

## 10. 展示講稿

### 2 分鐘影片

| 時間 | 畫面 | 講什麼 |
| --- | --- | --- |
| 0:00–0:15 | 報告「應用介紹」 | 「這是遠端主機狀態查詢服務，用 C 實作，server 每條連線 fork 一個子程序。」 |
| 0:15–0:35 | 連線時序圖 | 「父程序接連線、檢查名額、fork；子程序處理請求；結束後父程序用 waitpid 回收。」 |
| 0:35–0:50 | 協定表格 | 「12 bytes header，含 Magic、版本、OpCode、長度和 CRC32，全部 network byte order。」 |
| 0:50–1:20 | 跑 `./demo.sh` | 「10 個 client 同時連線，有 10 個不同的子程序。第 11 個收到 BUSY。」 |
| 1:20–1:45 | 第 10、11、13 階段 | 「關掉機制後，殭屍從 0 變 5，閒置連線不會釋放，重啟會 bind 失敗。」 |
| 1:45–2:00 | tcpdump 與總結 | 「封包裡看得到 4e53 和 OpCode。最後 34 項全部通過，壓力測試到 512 條上限。」 |

### 課堂現場

先在**自己的帳號**跑過一次，並在開始前執行 `sudo -v`。

```bash
sudo -v
./demo.sh
```

如果想手動展示：

```bash
make debug
./server -d -m 12 -t 30
```

```bash
./client -q all
./client -q ping
```

萬一現場出錯：

- `Address already in use`：另一個 server 還在跑，換個 port，例如 `./demo.sh 19191`。
- `缺少工具`：執行 `sudo apt-get install -y build-essential tcpdump`。
- 找不到 `.so`：確認 `lib/` 和執行檔在一起，用 `make clean && make` 重建。

---

## 11. 老師可能問的問題

**Q：為什麼用 fork 不用 thread？**
題目規定不能用 thread。優點是記憶體隔離，一個連線崩潰不影響其他連線；缺點是每條連線成本較高。

**Q：`fork` 後父子共用什麼？**
子程序得到記憶體的副本（實際用 copy-on-write），並繼承開啟的 fd。fd 指向同一個核心物件，所以兩邊都要關掉自己不用的 fd。

**Q：SIGCHLD 可以用 `SIG_IGN` 嗎？**
Linux 上可以自動回收，但這樣就無法更新連線計數，所以用 handler。

**Q：`SA_RESTART` 是做什麼的？**
讓被訊號打斷的系統呼叫自動重啟。`select` 例外，不管有沒有設都會回傳 `EINTR`，所以程式要自己處理。

**Q：`SA_NOCLDSTOP` 呢？**
子程序只是暫停時不要送 SIGCHLD，只有真正結束才通知。

**Q：為什麼 `SO_REUSEADDR` 不會有安全問題？**
它允許綁定還有 TIME_WAIT 連線的 port，不是讓兩個程式同時監聽同一個 port。

**Q：TCP 是串流，怎麼知道封包邊界？**
先讀固定 12 bytes 的 header，再依 Length 讀 payload。`recv_full` 會一直讀到湊滿為止，因為一次 `recv` 可能只拿到部分資料。

**Q：CRC 能防駭客嗎？**
不能。CRC 只偵測意外錯誤，攻擊者改完資料可以重算。要防竄改得用 HMAC，要防竊聽得用 TLS。

**Q：Client 一直傳資料但不讀回應會怎樣？**
Server 的 `send` 有 `SO_SNDTIMEO` 期限，逾時後子程序結束，不會永遠佔位。

**Q：連線上限設在哪？為什麼是 512？**
`-m` 可以設定，預設 32，程式上限是 512，也就是 `g_children` 追蹤表的大小。

**Q：系統最大的弱點是什麼？**
明文傳輸、沒有身分驗證，以及單一來源可以佔滿名額。預設只綁 `127.0.0.1` 來降低風險，要對外開放必須先處理這些。

---

## 12. 不要誇大的事

這些說法要照實講，被追問才站得住腳：

- 跨網路測試是 Windows 到 WSL2 的**虛擬網卡**，不是兩台實體電腦。
- 12,000 req/s 是 **Python client 測到的數字**，不是 server 的極限。
- 每輪壓力測試只有幾秒，**沒有做長時間穩定性測試**。
- Sanitizer 和模糊測試沒發現錯誤，**不代表沒有漏洞**。
- 測試是用 root 跑的，一般帳號的 sudo 流程要你自己先跑一次確認。

---

## 13. 自我檢查

合上文件，試著回答：

1. 為什麼 `on_sigchld` 必須用迴圈？
2. 父子程序各要關掉哪個 fd？為什麼？
3. `fork` 前為什麼要擋訊號？
4. idle timeout 為什麼整個封包只算一個期限？
5. TIME_WAIT 為什麼會讓重啟失敗？
6. Header 的 `0x88` 代表什麼？
7. `$ORIGIN` 解決了什麼問題？
8. 你測出的兩個問題是什麼？怎麼驗證修好了？

八題都能用自己的話講出來，現場被問就沒問題了。
