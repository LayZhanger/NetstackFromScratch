#ifndef NETDEV_H
#define NETDEV_H

#include <stddef.h>
#include <stdint.h>

/*
 * netdev：设备抽象基类（01-架构.md §3）。
 * 无任何协议语义：只做整帧收发与事件分派，不做地址/格式/分流。
 * 运行时模型（01-架构.md §2，线程化版）：后端内部起一个事件线程，
 * 收帧经 rx_handler 回调上送、发送入队由线程的 POLLOUT 事件驱动写出；收发均非阻塞。
 * netdev 无对应 RFC，代码溯源 01-架构.md §1 职责边界与 §2 运行时模型。
 */

/* 单帧容量上限（收帧缓冲按此大小分配） */
#define NETDEV_BUFSIZE 2048

struct netdev;

struct netdev_ops {
	int  (*init)(struct netdev *self);        /* 建 fd（O_NONBLOCK） */
	/*
	 * start：启动内部事件线程（阻塞等待收帧、POLLOUT 驱动发送）。
	 * 收帧经 rx_handler 回调上送；对端关闭/致命错误时线程退出。
	 */
	int  (*start)(struct netdev *self);
	int  (*send)(struct netdev *self, const uint8_t *buf, size_t len);  /* 入队 txq，非阻塞 */
	/*
	 * stop：停内部线程（join）、关闭 fd、释放资源。
	 * netdev_destroy 内部调用，需与 start 配对（c-style skill §4）。
	 */
	void (*stop)(struct netdev *self);
};

/*
 * 收帧回调：数据到达时由内部线程调用（线程上下文）。
 * buf 由 netdev 按帧 malloc（容量 NETDEV_BUFSIZE，len 为实际帧长），
 * 回调拥有所有权，处理完必须 free(buf)；回调须线程安全（可能在多线程中执行）。
 */
typedef void (*netdev_rx_handler_t)(struct netdev *self, uint8_t *buf,
				    size_t len);

struct netdev {
	const struct netdev_ops *ops;   /* 虚表，约定放首成员（c-style skill §3） */
	const char *name;               /* 设备名，如 "hub0" */
	size_t   rx_bytes;              /* 收到的总字节数 */
	size_t   tx_bytes;              /* 发送入队的总字节数 */
	netdev_rx_handler_t rx_handler; /* 数据到达回调，上层注册 */
};

/*
 * 生命周期：netdev_create → netdev_set_rx_handler → netdev_start → 使用 → netdev_destroy。
 * netdev_destroy 内部调 stop（stop+free，malloc/free 对称）。
 */

/* 分派封装：多态调用统一入口（内部 self->ops->xxx） */
int     netdev_init(struct netdev *self);
int     netdev_start(struct netdev *self);
int     netdev_send(struct netdev *self, const uint8_t *buf, size_t len);
void    netdev_stop(struct netdev *self);

/* 注册收帧回调（每个实例一个；须在 start 前注册） */
void    netdev_set_rx_handler(struct netdev *self, netdev_rx_handler_t h);

#endif /* NETDEV_H */
