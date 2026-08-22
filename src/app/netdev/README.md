# netdev 层 demo（阶段零）

协议栈进程经 netdev（unix_socket_netdev 后端，CLIENT 角色）与 hub 的收发演示：验证「协议栈进程 ↔ hub 收发成功」（docs/02-netdev与hub.md §1）。

## 构建

```bash
./build.sh
```

产物：`dist/bin/hub`（虚拟交换机）、`dist/bin/netdev_demo`（本 demo）。

## 运行（三个终端）

```bash
# 终端 1：启动 hub（虚拟交换机，固定 2 端口，局域网共享介质）
dist/bin/hub /tmp/netstack-hub 2

# 终端 2：进程 A（连端口0）
dist/bin/netdev_demo /tmp/netstack-hub-0.sock A

# 终端 3：进程 B（连端口1）
dist/bin/netdev_demo /tmp/netstack-hub-1.sock B
```

一键启动（自动构建 + 三个进程 + 聚合日志，Ctrl+C 全部停止）：

```bash
scripts/netdev/run-demo.sh              # 无丢包
scripts/netdev/run-demo.sh 0 40         # 端口1 丢 40%
```

## 参数

```
hub <sock_base> <nports> [丢包率0% 丢包率1% ...]   # 每端口一个 socket 路径 <base>-<i>.sock
netdev_demo <hub_port_socket_path> <名字A|B> [发送内容]
```

例：端口 1 丢 40% 帧（确定性测试）：

```bash
dist/bin/hub /tmp/netstack-hub 2 0 40
```

## 预期行为

- 两个 demo 进程每 1 秒互发互收：A 发 `hello-from-A`、B 发 `hello-from-B`，收到对端帧即打印。
- hub 打印转发日志：`端口0 -> 端口1 len=12 68 65 6c 6c ...`（hexdump，Wireshark 对照用）。
- Ctrl+C 任意进程，另一进程照常收发。

## 运行时模型（01-架构.md §2，后端事件线程）

- **收**：netdev 内部事件线程收到帧后调 `rx_handler` 回调（demo 里打印 + free），主循环不碰帧数据。
- **发**：`netdev_send` 只入队不阻塞，实际写出由内部线程的 POLLOUT 事件驱动。
- **周期发送**：主循环 `sleep` 后执行 `netdev_send`，与收帧线程互不干扰。

```
netdev_create("unix_socket", {sock_path, name, role=CLIENT})
netdev_set_rx_handler(nd, demo_rx_handler)   # 收帧回调：打印 + free
netdev_start(nd)                             # 起内部收帧线程
loop:
    sleep(1s)
    netdev_send(nd, 本机内容)                 # 入队，由内部线程 POLLOUT 写出
```
