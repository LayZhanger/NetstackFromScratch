# netdev 层 demo（阶段零）

协议栈进程经 netdev（hubport 后端）与 hub 的收发演示：验证「协议栈进程 ↔ hub 收发成功」（docs/02-netdev与hub.md §1）。

## 构建

```bash
./build.sh
```

产物：`dist/bin/hub`（虚拟交换机）、`dist/bin/netdev_demo`（本 demo）。

## 运行（三个终端）

```bash
# 终端 1：启动 hub（虚拟交换机，局域网共享介质）
dist/bin/hub /tmp/netstack-hub.sock

# 终端 2：进程 A
dist/bin/netdev_demo /tmp/netstack-hub.sock A

# 终端 3：进程 B
dist/bin/netdev_demo /tmp/netstack-hub.sock B
```

一键启动（自动构建 + 三个进程 + 聚合日志，Ctrl+C 全部停止）：

```bash
scripts/netdev/run-demo.sh              # 无丢包
scripts/netdev/run-demo.sh 0 40         # 端口1 丢 40%
```

## 参数

```
hub <socket_path> [丢包率0% 丢包率1% ...]     # 丢包率按连接注册顺序对应端口
netdev_demo <hub_socket_path> <名字A|B> [发送内容]
```

例：端口 1 丢 40% 帧（确定性测试）：

```bash
dist/bin/hub /tmp/netstack-hub.sock 0 40
```

## 预期行为

- 两个 demo 进程每 1 秒互发互收：A 发 `hello-from-A`、B 发 `hello-from-B`，收到对端帧即打印。
- hub 打印端口上下线与转发日志：`端口0 -> 端口1 len=12 68 65 6c 6c ...`（hexdump，Wireshark 对照用）。
- Ctrl+C 任意进程，另一进程照常收发。

## 事件驱动模型（01-架构.md §2）

- **收**：数据到达时 netdev 内部调 `rx_handler` 回调（demo 里打印 + free），主循环不碰帧数据。
- **发**：`netdev_send` 只入队不阻塞，实际写出由 `netdev_poll` 的 POLLOUT 事件驱动。
- **周期定时**：主循环 `netdev_poll(距下次周期 ms)` 超时返回 0 时执行发送，与收包互不干扰。

```
netdev_set_rx_handler(nd, demo_rx_handler)   # 收帧回调：打印 + free
loop:
    ret = netdev_poll(nd, 距下次周期发送 ms)
    if ret == 0:                             # 无帧事件，周期定时触发
        netdev_send(本机内容)                 # 入队，由 POLLOUT 写出
    if ret < 0:                              # 链路断开
        退出
```
