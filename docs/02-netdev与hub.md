# 阶段零 · netdev 裸收发 + hub 虚拟交换机

> 文档定位：阶段零的设计文档。代码按本文件写，标注与 01-架构.md 对应章节。
> 本阶段范围：netdev 基类接口 + hub 端口后端（hubport）+ hub 虚拟交换机（C 实现）+ 演示进程。
> 范围外：tap 端口（与 Linux 内核互操作留到 ARP/IP/ICMP 里程碑前再加）、帧格式/分流（eth 阶段）。

## 1. 目标与验证标准

- **里程碑零定义**：协议栈进程经 netdev 接口与 hub **收发成功**。
  - 收：hub 转来的帧能被 `netdev_read` 完整读到。
  - 发：`netdev_send` 写出的帧被 hub 完整收到并转发（对端进程可见）。
- **验证路径**：进程 A 经 netdev_send 发出 → hub 收到并转发 → 进程 B 经 netdev_read 读到并打印。
  - 进程 A 的 send 成功 = A↔hub 上行链路通。
  - 进程 B 的 read 成功 = B↔hub 下行链路通（帧经 hub 转达）。
  - 双进程对发即证明每个协议栈进程与 hub 的收发均成功。
- 本阶段 netdev 不做任何协议语义（无地址/格式/分流），对照 01-架构.md §1 职责边界。

## 2. 拓扑图

```
                ┌───────────────────────────────────────────────┐
                │                 hub 进程 (hub)                 │
                │             虚拟交换机 · 仅 L2 搬运             │
                │                                               │
                │   listen: /tmp/netstack-hub.sock              │
                │                                               │
                │   ┌─────────┐          ┌─────────┐            │
                │   │  端口A   │          │  端口B   │            │
                │   │ (fd1)   │          │ (fd2)   │            │
                │   └────┬────┘          └────┬────┘            │
                └────────┼────────────────────┼─────────────────┘
                         │ unix socket        │ unix socket
                         │ (accept 连接)      │ (accept 连接)
            ┌────────────┼─────────────┐   ┌──┼────────────────┐
            │ 协议栈进程 A              │   │ 协议栈进程 B        │
            │                           │   │                    │
            │  hubport_netdev (fd)      │   │ hubport_netdev (fd)│
            │       ↕ connect           │   │      ↕ connect     │
            │  netdev_read/send         │   │ netdev_read/send   │
            └───────────────────────────┘   └────────────────────┘

   帧流向（A → B）：
   A 进程: netdev_send(fdA, 帧)
       │  写 unix socket
       ▼
   hub:   读 fd1 得整帧 → 转发写 fd2
       │  写 unix socket
       ▼
   B 进程: netdev_read(fdB) 读到完整帧
```

- 每台设备 = 一个 C 进程，各自持有独立 hubport_netdev 实例，互不感知，只和 hub 通信（对照 01-架构.md §4）。
- 通信介质：Unix domain socket（本地局域网链路模拟）；后续 hub 增加 tap 端口时，协议栈侧接口不变（对照 01-架构.md §3 抽象原则）。

## 3. netdev 基类接口

位置：`src/netdev/netdev.h`。接口与 01-架构.md §3 一致：

```c
struct netdev;
struct netdev_ops {
    int      (*init)(struct netdev *self);
    int      (*poll)(struct netdev *self, int timeout_ms);   /* 等待并处理收/发事件 */
    int      (*send)(struct netdev *self, const uint8_t *buf, size_t len); /* 入队 */
    void     (*periodic)(struct netdev *self);               /* 周期定时器 */
};

/* 收帧回调：buf 由 netdev 按帧 malloc，回调拥有所有权，处理完必须 free(buf) */
typedef void (*netdev_rx_handler_t)(struct netdev *self, uint8_t *buf, size_t len);

struct netdev {
    const struct netdev_ops *ops;   /* 虚表，约定放首成员（c-style skill） */
    const char *name;               /* 设备名，如 "hub0" */
    size_t   rx_bytes;              /* 收到的总字节数 */
    size_t   tx_bytes;              /* 发送入队的总字节数 */
    netdev_rx_handler_t rx_handler; /* 数据到达回调，netdev_set_rx_handler 注册 */
};
```

约定（对照 c-style skill）：
- ops 表 `static const`，按实例指定（基类不持有设备表，进程单实例栈）。
- `self` 首参，多态分派：`nd->ops->send(nd, ...)`。
- 子类首成员内嵌 `struct netdev`，`container_of(nd, struct hubport_netdev, base)` 反推子类。
- `poll` 语义：等待并处理收/发事件，返回 `1` = 有活动（收帧回调已处理 / 队列已写出），`0` = 超时无事件（主循环转周期定时），`-1` = 错误或链路断开。
- `send` 语义：入队（拷贝）即返回 `len`，不阻塞；队列分配失败或帧超 `NETDEV_BUFSIZE` 返回 -1。
- netdev 无对应 RFC（无协议语义），代码注释溯源 01-架构.md §1/§2/§3。

## 4. hubport 后端

位置：`src/netdev/hubport.c`。

```c
struct txq;                       /* 发送队列，定义藏于 hubport.c */
struct hubport_netdev {
    struct netdev base;           /* 继承：首成员内嵌基结构 */
    int  fd;                      /* 连 hub 的 unix socket（O_NONBLOCK） */
    struct txq *txq;              /* 发送队列（动态分配） */
};
```

- 初始化：`socket(AF_UNIX, SOCK_STREAM, 0)` → `connect(hub_sock_path)`（连接即向 hub 注册），fd 设 `O_NONBLOCK`；销毁：`hubport_netdev_destroy` 关闭 fd、释放队列（malloc/free 对称）。
- **收**：`poll` 检测 POLLIN → `malloc(NETDEV_BUFSIZE)` 缓冲 → read → 调 `rx_handler(self, buf, n)` 上送（回调内 free）；`read` 返回 0 或错误视为链路断开，`poll` 返回 -1。
- **发**：`send` 入队（malloc 节点 + 帧缓冲并拷贝），返回 `len`；`poll` 检测 POLLOUT 驱动写出，部分写记录 `off` 续写，`EAGAIN` 留队头等下个 POLLOUT 自动重试，写完 free 出队。
- periodic：本阶段无定时业务，预留空实现（后续 ARP 老化/QUIC 重传用）。
- 统计：rx_bytes 收帧时累加，tx_bytes 入队时累加。

## 5. hub 虚拟交换机（C）

位置：`hub/hub.c`（独立目录，与 src/、tests/ 平级：hub 是独立进程的局域网仿真介质，不属于协议栈也不属于测试用例）。多端口用 `select` 单线程驱动（无复杂并发，对照 AGENTS.md 设计取舍）。

- 启动：`hub <socket_path>`，`bind + listen` unix socket，连接即端口注册。
- 端口表：静态数组（编译期定最大端口数，本阶段 8）。
- 转发规则（本阶段简化版，对照 01-架构.md §4 完整规则的子集）：
  - 收到某端口一帧 → 原样写往**其它所有端口**（共享介质语义；广播/组播/单播细分留到 eth 阶段）。
- 丢包开关（确定性测试用，01-架构.md §4）：`hub <socket_path> <丢包率A%> <丢包率B%>`，按端口概率丢弃，命令行列端口号与丢包率。
- 抓包日志：每收/发一帧打印 `[ts] 端口X → 端口Y len=N` + 十六进制字节，便于对照。

## 6. demo 进程

位置：`src/app/netdev/demo.c`。阶段零演示，验证协议栈进程与 hub 收发成功。

- 启动：`demo <hub_socket_path> <名字A|B> [发送内容]`。
- 注册 `rx_handler`：收到帧打印字节并 `free(buf)`（回调拥有缓冲所有权）。
- 主循环（对照 01-架构.md §2 事件驱动）：

```
netdev_set_rx_handler(nd, demo_rx_handler)   # 收帧回调：打印 + free
loop:
    ret = netdev_poll(nd, 距下次周期发送 ms)
    if ret == 0:                             # 无帧事件，周期定时触发
        netdev_send(本机内容)                 # 入队，由 poll 的 POLLOUT 写出
        排下次发送时间
    if ret < 0:                              # 链路断开
        退出
```

- 进程 A 定时发 "hello-from-A"，进程 B 定时发 "hello-from-B"，互发互收。

## 7. 单元测试

位置：`tests/unit/test_hubport.c`（check 框架，接 tests/CMakeLists.txt）。

- socketpair mock 链路（一端连 hubport fd，一端由测试持有），无需真实 hub：
  - 收：对端写入 → `poll` 返回 1、`rx_handler` 回调收到完整帧（断言后 free）、rx_bytes 累加；连续多帧各自独立缓冲；无数据超时返回 0；对端关闭返回 -1。
  - 发：`send` 入队立即返回 len（不阻塞）→ `poll` 的 POLLOUT 驱动写出 → 链路对端到齐；多帧顺序正确；帧超 `NETDEV_BUFSIZE` 拒绝。
  - ops 多态分派、container_of 反推子类 fd、构造失败路径。
- 与 01-架构.md §4 的验证体系对应：单测 = 开发期逻辑正确性，手动演示 = 真实 hub 双进程互通。

## 8. 手动演示步骤

```
# 终端 1：启动 hub（无丢包）
hub/hub /tmp/netstack-hub.sock

# 终端 2：启动进程 A
dist/bin/netdev_demo /tmp/netstack-hub.sock A hello-from-A

# 终端 3：启动进程 B
dist/bin/netdev_demo /tmp/netstack-hub.sock B hello-from-B
```

一键启动（自动构建 + 三进程 + 聚合日志，Ctrl+C 全部停止）：`scripts/netdev/run-demo.sh [hub 丢包率参数...]`。

预期：
- A 定时打印收到 "hello-from-B"，B 定时打印收到 "hello-from-A"。
- hub 日志出现 `端口A → 端口B` 与 `端口B → 端口A` 的转发记录，长度与发送内容一致。
- 任一进程 Ctrl+C，另一进程照常收发（链路隔离正确）。

## 9. 里程碑确认清单与 tag

| 项 | 确认 |
|----|------|
| netdev 基类接口与 01-架构.md §3 一致 | ✔ |
| hubport 经 fd 与 hub 收发成功 | ✔ |
| hub 端口注册 + 转发 + 丢包开关 | ✔ |
| demo 双进程经 hub 互发互收 | ✔ |
| check 单测通过（build.sh，11 用例） | ✔ |
| 手动演示三终端跑通 | ✔ |

全部通过，已打 git tag `netdev`。构建与运行：`./build.sh`；手动演示见 §8（编译产物在 `dist/bin/`）。
