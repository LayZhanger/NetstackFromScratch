# netstack-from-scratch · 手搓网络协议栈

对照 RFC 原文，从零用 C 手写一套网络协议栈：**以太网 → ARP → IP → ICMP → UDP → QUIC → HTTP/3**。

> 配套视频：B站/抖音「Lay 的 RFC 实验室」同步更新，从 RFC 原文一行一行拆解实现。

## 目标与原则

- **自底向上、分层递进**：netdev → 链路层 → 网络层 → UDP → QUIC → 应用层。
- **每步最小可运行**：一步只做一个可验证功能，做完立刻能抓包/单测验证。
- **每行代码可溯源 RFC**：核心结构体和关键函数逐行标注 RFC Section 编号。
- **抽象优先**：netdev 层屏蔽底层设备差异，协议栈只依赖接口，不依赖具体设备。

## 范围

- ✅ 本仓库：协议栈核心 + 基于自研 QUIC 的 HTTP/3 服务
- ❌ 不实现 TCP（RFC 793）——传输层直接做 QUIC（RFC 9000 系列）

## 目录结构

```
netstack-from-scratch/
├── docs/           计划与架构文档
├── src/
│   ├── include/    types.h byteorder.h checksum.h
│   ├── eth/  arp/  ip/  icmp/  udp/  quic/   ← 按层（抽象接口在根，具体实现进 impl/）
│   ├── netdev/     netdev.h + 工厂 + 实现（unix_socket/tap 后端，内部线程驱动）
│   └── app/       按层 demo（netdev/ eth/ ...）+ echo/http
├── tests/          link/hub 脚本 + Python 单测
├── hub/            hub 虚拟交换机（复用 netdev 作端口，独立进程；含 tap 端口支持）
└── scripts/        tap 配置等辅助脚本
```

## 各层与 RFC 对照

| 层 | 协议 | RFC |
|----|------|-----|
| 链路层 | 以太网帧 | 894 |
| 链路层 | ARP | 826 |
| 网络层 | IP | 791 |
| 网络层 | ICMP | 792 |
| 传输层 | UDP | 768 |
| 传输层 | QUIC | 9000/9001/9002 |
| 应用层 | HTTP/3 | 9114 |

## 文档

- `docs/00-计划.md` — 总体路线图（阶段、功能清单、依赖、里程碑）
- `docs/01-架构.md` — 总体架构设计（分层、netdev 抽象、hub 虚拟局域网）
