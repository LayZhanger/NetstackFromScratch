#ifndef NETDEV_H
#define NETDEV_H

#include <stddef.h>
#include <stdint.h>

/*
 * netdev：设备抽象基类（01-架构.md §3）。
 * 无任何协议语义：只做整帧收发与事件分派，不做地址/格式/分流。
 * 事件驱动模型（01-架构.md §2）：收帧走回调（rx_handler），发送入队由 poll 驱动写出。
 * netdev 无对应 RFC，代码溯源 01-架构.md §1 职责边界与 §2 运行时模型。
 */

/* 单帧容量上限（收帧缓冲按此大小分配） */
#define NETDEV_BUFSIZE 2048

struct netdev;

struct netdev_ops {
	int      (*init)(struct netdev *self);
	/*
	 * poll：事件驱动入口，等待并处理收/发事件。
	 * 返回：1 = 有活动（收帧回调已处理完 / 队列已写出），0 = 超时无事件
	 *       （主循环转周期定时），-1 = 错误或对端关闭。
	 */
	int      (*poll)(struct netdev *self, int timeout_ms);
	int      (*send)(struct netdev *self, const uint8_t *buf, size_t len);
	void     (*periodic)(struct netdev *self);
};

/*
 * 收帧回调：数据到达时由 poll 内部调用。
 * buf 由 netdev 按帧 malloc（容量 NETDEV_BUFSIZE，len 为实际帧长），
 * 回调拥有所有权，处理完必须 free(buf)。
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

/* 分派封装：多态调用统一入口（内部 self->ops->xxx） */
int     netdev_init(struct netdev *self);
int     netdev_poll(struct netdev *self, int timeout_ms);
int     netdev_send(struct netdev *self, const uint8_t *buf, size_t len);
void    netdev_periodic(struct netdev *self);

/* 注册收帧回调（每个实例一个） */
void    netdev_set_rx_handler(struct netdev *self, netdev_rx_handler_t h);

#endif /* NETDEV_H */
