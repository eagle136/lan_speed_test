# 协议与实现细节

面向改代码的人。只想用工具的话看 [README](../README.md) 就够了。

所有实现集中在 `core/`，PC 端（`pc/pc_main.c`）与安卓端（`android/app/src/main/jni/lanspeed_jni.c`）
只是把同一份 core 包进各自的 UI。

---

## 1. 端口

| 端口 | 协议 | 用途 |
|---|---|---|
| 5310 | UDP | 设备自动发现（广播查询 + /24 单播扫描） |
| 5201–5204 | TCP | 4 条并行测速流，一条流一个会话 |

`LS_DEFAULT_PORT` 是 5201，其余 3 条由调用方 `port + i` 得到（见 `pc_main.c` 的 `PORT + i`）。

## 2. 角色判定（无协商冲突）

两端的 `decide_role()` 逻辑必须逐字一致：

```
c = cmp_ip(local_ip, peer_ip)          // IPv4 按数值比较，不是字符串
if c == 0: c = strcmp(self_id, peer_id) // IP 相同（本机回环测试）再比设备 ID
if c == 0: c = 1
role = (c < 0) ? LS_ROLE_PHONE(监听) : LS_ROLE_PC(连接)
```

- 两端对同一对数得出的 `c` 互为相反数，所以**必然**一端监听、一端连接。
- 设备 ID 是 14 位十六进制，启动时由 pid / 时钟 / 栈地址混合生成，不落盘（见 `make_id()`）。
- IP 完全相同且 ID 也相同（自己连自己）时强制 `c = 1`，落到 `LS_ROLE_PC`。

## 3. 报文格式

所有结构 `#pragma pack(1)`，窗口大小固定 16 字节，直接 memcpy。

### 3.1 HELLO（握手，16 字节）

| 偏移 | 类型 | 字段 | 值 |
|---|---|---|---|
| 0 | u32 | magic | `0x4C534831`（"LSH1"） |
| 4 | u8 | type | `1` = HELLO |
| 5 | u8 | role | 1 = PC(连接端)，2 = PHONE(监听端) |
| 6 | u16 | version | `LS_VERSION`，当前 1 |
| 8 | u32 | seconds | 单方向时长，接收方校验 1–600 |
| 12 | u32 | reserved | 0 |

握手的硬性检查（`do_hello()`）：

- magic / type / version 任一不符 → `handshake mismatch`
- 对端 role 必须等于自己的反面 → 否则 `unexpected peer role`
- 连接端采用监听端上报的 `seconds`（监听端是权威方）

### 3.2 帧头（16 字节，每个 1 MiB 块的开头）

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u32 | magic `0x4C535431`（"LST1"） |
| 4 | u8 | type：1 HELLO / 2 START / 3 END / 4 DATA / 5 DONE |
| 5 | u8 | phase：1 上行、2 下行 |
| 6 | u16 | reserved |
| 8 | u32 | seq（DATA 帧递增序号） |
| 12 | u32 | value（控制帧参数） |

三种控制帧（START / END）和 DONE 帧都是**整块 1 MiB**发送，只有前 16 字节有意义——
这不是浪费，是为了让控制帧与数据帧共用同一条定长读取路径，接收侧不需要状态机分叉。

### 3.3 DATA 帧

- 整块 1 MiB，第 0–15 字节是帧头。
- 偏移 16 处写入 `seq`，块尾最后 4 字节也写入 `seq`（`recv_phase()` 里的 `tag1` / `tag2`）。
  接收端只统计 `errors`，**不中断传输**——测吞吐不拿校验和当门禁，但链路串包能立刻看出来。
- 载荷固定 pattern：`b0` 用 `0xA5`、`b1` 用 `0x5A` 预填充，双缓冲轮换。
  固定 pattern 让发送侧不需要每次 memset，也便于 hexdump 排障。

### 3.4 DONE 帧

帧头后面紧跟 `ls_result_t`：`u64 bytes` + `u64 ns`，即发送端自己统计的该方向实测值。
接收端会与本地计数交叉验证，最终**以接收端为准**（见 `recv_done()`；`ns == 0` 视为无效）。

## 4. 一轮测速的时序

```
监听端(phone)                      连接端(pc)
  listen 5201..5204
                     <----------    connect 5201..5204
  HELLO            ---------->    
                   <----------    HELLO
  [阶段1] 持续发送 1 MiB DATA 块 → 
  END(phase=1)
  DONE(phase=1, bytes, ns)
  [阶段2] 循环 recv ← 1 MiB DATA 块
                   <----------    END(phase=2)
                   <----------    DONE(phase=2)
  汇总 → DONE
```

`run_phone()` / `run_pc()` 是镜像实现：phone 先发后收，pc 先收后发。
两个阶段之间没有 sleep，切换靠控制帧。

## 5. 统计口径

| 项 | 定义 |
|---|---|
| `up_mbps` / `down_mbps` | `bytes * 8 / ns * 1e9 / 1e6`，bytes 与 ns 都来自**接收端** |
| `cur_mbps_raw` | 200 ms 窗口瞬时值（`LS_RATE_WINDOW`） |
| `cur_mbps` | 最近 8 个窗口的滑动平均（`LS_RATE_SAMPLES`） |
| `progress` | 整轮口径：每条流占 [0, 0.5) 上行、[0.5, 1] 下行 |
| `LS_STALL_NS` | 10 s 无任何字节 → 判定该流卡死 |
| `LS_GAP_LOG_NS` | 单次 recv 间隔 > 50 ms 记一条 gap 日志 |

UI 抖动来自「用短窗口值刷新」。所以对外只暴露滑动平均，原始 200 ms 值只进日志。
`tests/ls_selftest.c` 会断言两端 `up_mbps` / `down_mbps` 差值 < 0.01 Mbps——统计口径改错时立刻炸。

## 6. 发送流水线（实验特性）

`ls_session_set_pipeline(s, 1)` 打开后，发送阶段拆成两个线程：

- 生产者线程：准备下一块 buffer（打帧头、写 seq），双槽环形交接
- I/O 线程：只做 `send()`

用途是 A/B 判断瓶颈在「准备数据」还是「内核发送」。默认关闭，PC/安卓 UI 上都有勾选框。
打开后如果数字没变，说明瓶颈在空口或接收侧，不在用户态。

## 7. 自动发现（UDP 5310）

报文是纯文本，一行一个：

```
LSDISC1|id=<14 hex>|port=5201|q=<0|1>|name=<utf8, ≤63B>\n
```

| 字段 | 含义 |
|---|---|
| `id` | 设备 ID，用于角色判定平局时比较、以及去重 |
| `port` | 对端 TCP 起始端口（固定 5201） |
| `q` | 1 = 主动查询，0 = 应答/被动信标 |
| `name` | 显示名（PC 传主机名，安卓传机型） |

流程：

1. 启动时开一个 UDP 监听线程（后台常驻），同时发一次被动信标。
2. 点「扫描」→ 广播 `q=1` 查询 + 对本网段 /24 逐个单播（`DISC_RECV_MS` 300 ms/轮）。
3. 收到 `q=1` 的对端报文，回一个 `q=0` 应答；同一 IP 在 `DISC_REPLY_MS`（2 s）内只回一次，
   去重表 16 槽。
4. 对端信息在 `DISC_FRESH_MS`（10 s）内视为有效，最多记 8 个（`LS_DISC_MAX_PEERS`）。

单播扫描的意义：很多企业 AP / 家用路由器开了 AP 隔离，广播包出不去，但同一 /24 内的
单播能通（或反之）。两条路都走，命中率明显高于纯广播。

## 8. 已知取舍

- 不做 TLS / 认证：局域网内测速工具，加了只会把 CPU 变成瓶颈，测出来的数字失去意义。
- 不做逐字节校验：会吃掉 1–2 Gbps 级别的 CPU 预算。改为 seq 抽样 + pattern 预填。
- 不做 IPv6：链路本地地址的角色比较规则和 scope id 处理会让实现复杂度翻倍，收益极低。
- `st_fail()` 里的错误文案直接面向用户展示，改文案要同时检查两端 UI 的拼接逻辑。
