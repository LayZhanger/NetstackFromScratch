#ifndef HUPPORT_NETDEV_H
#define HUPPORT_NETDEV_H

#include "netdev.h"

/*
 * hubport：netdev 的 hub 端口后端（docs/02-netdev与hub.md §4）。
 * 继承 netdev（首成员内嵌基结构），经 unix socket 连 hub。
 * 事件驱动：收帧经 rx_handler 回调上送；发送入队，由 poll 的 POLLOUT 事件写出。
 * 实例由外部定义（demo/单测），本头文件暴露完整 struct 与构造/销毁 API。
 * txq 定义藏于 hubport.c（私有实现），struct 里只放指针。
 */

struct txq;

struct hubport_netdev {
	struct netdev base;     /* 继承：首成员内嵌基结构 */
	int  fd;                /* 连 hub 的 unix socket（非阻塞） */
	struct txq *txq;        /* 发送队列（动态分配，入队即拷贝） */
};

/*
 * 完整构造：建 socket 并 connect hub，成功返回 0，失败返回 -1。
 * sock_path：hub 的 unix socket 路径；name：设备名（如 "hub0"）。
 * fd 被设为 O_NONBLOCK。
 */
int  hubport_netdev_init(struct hubport_netdev *hp, const char *sock_path,
			 const char *name);

/*
 * 绑定已就绪 fd 构造（单测/复用场景）：不建 socket，直接接管 fd。
 * fd < 0 时返回 -1。
 */
int  hubport_netdev_attach(struct hubport_netdev *hp, int fd, const char *name);

/* 销毁：关闭 fd、释放发送队列（malloc/free 对称，c-style skill §4） */
void hubport_netdev_destroy(struct hubport_netdev *hp);

#endif /* HUPPORT_NETDEV_H */
