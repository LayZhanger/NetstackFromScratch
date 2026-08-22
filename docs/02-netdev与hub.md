# 阶段零 · netdev 裸收发 + hub 虚拟交换机

> 文档定位：阶段零的设计文档。代码按本文件写，标注与 01-架构.md 对应章节。
> 本阶段范围：netdev 基类接口 + unix_socket 后端（client/server 双角色）+ hub 虚拟交换机（复用 netdev）+ 演示进程。
> 范围外：tap 端口（与 Linux 内核互操作留到 ARP/IP/ICMP 里程碑前再加）、帧格式/分流（eth 阶段）。

## 1. 目标与验证标准

- **里程碑零定义**：协议栈进程经 netdev 接口与 hub **收发成功**。
  - 收：hub 转来的帧能被 `rx_handler` 回调完整收到。
  - 发：`netdev_send` 入队的帧被 hub 完整收到并转发（对端进程可见）。
- **验证路径**：进程 A 经 netdev_send 发出 → hub 收到并转发 → 进程 B 经 rx_handler 收到并打印。
  - 进程 A 的 send 成功 = A↔hub 上行链路通。
  - 进程 B 的收到帧 = B↔hub 下行链路通（帧经 hub 转达）。
  - 双进程对发即证明每个协议栈进程与 hub 的收发均成功。
- 本阶段 netdev 不做任何协议语义（无地址/格式/分流），对照 01-架构.md §1 职责边界。

## 2. 拓扑图

```
            hub 进程（虚拟交换机，仅 L2，复用 netdev）
  ┌─────────────────────────────────────────────┐
  │  hub_init 固定配置 N 端口（每端口 = netdev SERVER）
  │    ports[0]              ports[1]
  │   (netdev,SERVER)        (netdev,SERVER)
  │   path: base-0.sock      path: base-1.sock
  └───────┬───────────────────────┬─────────────┘
          │ unix socket            │ unix socket
          │ (accept 在 netdev 内)  │ (accept 在 netdev 内)
  ┌───────┴──────────┐      ┌──────┴───────────┐
  │ 协议栈进程 A      │      │ 协议栈进程 B      │
  │ netdev CLIENT    │      │ netdev CLIENT    │
  │  rx_handler/send │      │  rx_handler/send │
  └──────────────────┘      └──────────────────┘

  帧流向（A → B）：
  A: netdev_send(fdA, 帧)  ──写 socket──▶  hub 端口0 收到（rx_handler）
                                               └─▶ netdev_send(端口1) ──写 socket──▶ B 收到
```

- 每台设备 = 一个 C 进程，各自持有独立 netdev 实例（CLIENT 角色），互不感知，只和 hub 通信（对照 01-架构.md §4）。
- 通信介质：Unix domain socket（本地局域网链路模拟）；后续 hub 增加 tap 端口时，协议栈侧接口不变（对照 01-架构.md §3 抽象原则）。

## 3. netdev 基类接口

位置：`src/netdev/netdev.h`。接口与 01-架构.md §3 一致：

```c
struct netdev_ops {
    int  (*init)(struct netdev *self);          /* 建 fd（O_NONBLOCK） */
    int  (*start)(struct netdev *self);         /* 启动内部事件线程 */
    int  (*send)(struct netdev *self, const uint8_t *buf, size_t len); /* 入队，非阻塞 */
    void (*stop)(struct netdev *self);          /* 停线程、释放 */
};

/* 收帧回调：buf 由 netdev 按帧 malloc，回调拥有所有权，处理完必须 free(buf)（线程上下文） */
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
- 子类首成员内嵌 `struct netdev`，`container_of(nd, struct unix_socket_netdev, base)` 反推子类。
- 生命周期：`netdev_create → netdev_set_rx_handler → netdev_start → 使用 → netdev_destroy`（内部 stop+free）。
- `start` 语义：启动内部事件线程（收发均非阻塞）；`send` 入队（拷贝）即返回 `len`，不阻塞；队列分配失败或帧超 `NETDEV_BUFSIZE` 返回 -1。
- netdev 无对应 RFC（无协议语义），代码注释溯源 01-架构.md §1/§2/§3。

## 4. unix_socket 后端

位置：`src/netdev/impl/unix_socket_netdev.c`（netdev 的具体后端放 `impl/`）。支持 CLIENT/SERVER 双角色。

```c
struct txq;                       /* 发送队列，定义藏于 unix_socket_netdev.c */
struct unix_socket_netdev {
    struct netdev base;           /* 继承：首成员内嵌基结构 */
    int  fd;                      /* 已连接的 unix socket（O_NONBLOCK） */
    struct txq *txq;              /* 发送队列（动态分配，互斥锁保护） */
    int  role;                    /* UNIX_SOCK_ROLE_CLIENT / SERVER */
    int  running;                 /* 内部线程运行标志 */
    pthread_t rx_thread;          /* 内部收/发事件线程 */
    char sock_path[108];          /* SERVER 模式：listen 路径，关闭时 unlink */
};
```

- 创建：工厂 `netdev_create("unix_socket", &params)`（params 为 `unix_socket_params`，含 `sock_path`、`name`、`role`）。
  - **CLIENT**（栈进程）：内部 `socket(AF_UNIX)` + `connect(sock_path)` → fd 设 `O_NONBLOCK`。
  - **SERVER**（hub 端口）：内部 `socket + bind(sock_path) + listen + accept`（恰好一个对端）→ fd 设 `O_NONBLOCK`，记下 `sock_path` 供关闭时 unlink。
- **收**：内部线程 poll 检测 POLLIN → `malloc(NETDEV_BUFSIZE)` 缓冲 → read → 调 `rx_handler(self, buf, n)` 上送（回调内 free）；read 返回 0 或错误 → 线程退出（对端关闭）。
- **发**：`send` 入队（malloc 节点 + 帧缓冲并拷贝）返回 `len`；线程 poll 检测 POLLOUT 驱动写出，部分写记录 `off` 续写，`EAGAIN` 留队头等下个 POLLOUT 自动重试，写完 free 出队。txq 受互斥锁保护（多线程并发 send）。
- 统计：rx_bytes 收帧时累加，tx_bytes 入队时累加。

## 5. hub 虚拟交换机（复用 netdev）

位置：`hub/hub.c`（独立目录，与 src/、tests/ 平级：hub 是独立进程的局域网仿真介质，不属于协议栈也不属于测试用例）。**hub 直接复用 netdev，无端口抽象/工厂/后端，也无任何 socket 代码**——socket/accept/线程全在 netdev 内部。

```c
struct hub {
    struct netdev *ports[HUB_MAX_PORTS];  /* 端口 = netdev 实例（SERVER 角色） */
    int  nports;                          /* 已配置端口数（固定） */
    int  loss[HUB_MAX_PORTS];             /* 各端口丢包率 */
    unsigned long dropped[HUB_MAX_PORTS]; /* 各端口丢包计数 */
    char sock_base[...];                  /* 端口路径前缀 <base>-<i>.sock */
};
```

- 启动（固定配置）：`hub <sock_base> <nports> [丢包率...]`，`hub_init` 为每端口创建 server-role netdev（路径 `<sock_base>-<i>.sock`）并 `netdev_start`。
- **转发**：hub 的 `rx_handler`（某端口线程收到帧）→ 遍历其它端口 `netdev_send`（共享介质语义；广播/组播/单播细分留到 eth 阶段）。
- **丢包**：按 `loss[dst]` 概率丢弃（确定性测试用），命中则不经 `netdev_send` 直接计数并丢。
- `hub_run` 无需 select/accept：端口 netdev 各自内部线程驱动收发，主循环仅 `pause` 保活。
- 抓包日志：每收/发一帧打印 `[ts] 端口X → 端口Y len=N` + 十六进制字节，便于对照。

## 6. demo 进程

位置：`src/app/netdev/demo.c`。阶段零演示，验证协议栈进程与 hub 收发成功。

- 启动：`demo <hub_port_socket_path> <名字A|B> [发送内容]`（CLIENT 角色，`sock_path` 应为 `<base>-<i>.sock`）。
- 注册 `rx_handler`：收到帧打印字节并 `free(buf)`（回调拥有缓冲所有权）。
- **生命周期**：`netdev_create("unix_socket", {role=CLIENT,...})` → `netdev_set_rx_handler` → `netdev_start`（起内部线程）→ 主循环周期发送 → `netdev_destroy`。

```
netdev_create("unix_socket", {sock_path, name, role=CLIENT})
netdev_set_rx_handler(nd, demo_rx_handler)   # 收帧回调：打印 + free
netdev_start(nd)                             # 起内部线程（收发均非阻塞）
loop:
    sleep(1s)
    netdev_send(nd, 本机内容)                 # 入队，由内部线程 POLLOUT 写出
```

- 进程 A 定时发 "hello-from-A"，进程 B 定时发 "hello-from-B"，互发互收。

## 7. 单元测试

位置：`tests/unit/test_unix_socket_netdev.c`（check 框架，接 tests/CMakeLists.txt）。

- socketpair mock 链路（一端接 unix_socket fd，一端由测试持有），无需真实 hub：
  - 收：`start` 起线程 → 对端写入 → 线程 read → `rx_handler` 回调收到完整帧（断言后 free）；连续多帧各自独立缓冲；对端关闭线程退出。
  - 发：`send` 入队立即返回 len（不阻塞）→ 线程 POLLOUT 驱动写出 → 链路对端到齐；帧超 `NETDEV_BUFSIZE` 拒绝。
  - ops 多态分派、container_of 反推子类 fd、工厂（未知类型/NULL params）、构造失败路径。
- 与 01-架构.md §4 的验证体系对应：单测 = 开发期逻辑正确性，手动演示 = 真实 hub 双进程互通。

## 8. 手动演示步骤

```
# 终端 1：启动 hub（固定 2 端口，无丢包）
hub/hub /tmp/netstack-hub 2

# 终端 2：启动进程 A（连端口0）
dist/bin/netdev_demo /tmp/netstack-hub-0.sock A hello-from-A

# 终端 3：启动进程 B（连端口1）
dist/bin/netdev_demo /tmp/netstack-hub-1.sock B hello-from-B
```

一键启动（自动构建 + 三进程 + 聚合日志，Ctrl+C 全部停止）：`scripts/netdev/run-demo.sh [hub 丢包率参数...]`。

预期：
- A 定时打印收到 "hello-from-B"，B 定时打印收到 "hello-from-A"。
- hub 日志出现 `端口0 → 端口1` 与 `端口1 → 端口0` 的转发记录（rx_handler 转发），长度与发送内容一致。
- 任一进程 Ctrl+C，另一进程照常收发（链路隔离正确）。

## 9. 里程碑确认清单与 tag

| 项 | 确认 |
|----|------|
| netdev 基类接口与 01-架构.md §3 一致（init/start/send/stop） | ✔ |
| unix_socket 后端 CLIENT/SERVER 双角色 + 内部线程收发 | ✔ |
| hub 复用 netdev（无端口抽象/工厂/后端）+ 转发 + 丢包 | ✔ |
| demo 双进程经 hub 互发互收 | ✔ |
| check 单测通过（build.sh，test_ops 5 + test_unix_socket_netdev 10） | ✔ |
| 手动演示三终端跑通 | ✔ |

全部通过，已打 git tag `netdev`。构建与运行：`./build.sh`；手动演示见 §8（编译产物在 `dist/bin/`，需 `-pthread`）。
