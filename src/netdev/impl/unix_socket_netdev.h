#ifndef UNIX_SOCKET_NETDEV_H
#define UNIX_SOCKET_NETDEV_H

#include "netdev.h"
#include <pthread.h>

/*
 * unix_socket_netdev：netdev 的 unix socket 后端（docs/02-netdev与hub.md §4）。
 * 继承 netdev（首成员内嵌基结构），经 unix socket 连 hub。
 * 运行时（01-架构.md §2 线程化版）：内部事件线程，收帧经 rx_handler 回调上送、
 * 发送入队由线程的 POLLOUT 事件驱动写出，收发均非阻塞。
 * 支持两种角色：CLIENT = 栈进程（connect hub），SERVER = hub 端口（listen/accept）。
 * 实例由工厂 malloc（netdev_create），生命周期见 netdev.h。
 * txq 定义藏于 unix_socket_netdev.c（私有实现），struct 里只放指针。
 */

struct txq;

struct unix_socket_netdev {
	struct netdev base;     /* 继承：首成员内嵌基结构 */
	int  fd;                /* 已连接的 unix socket（O_NONBLOCK） */
	struct txq *txq;        /* 发送队列（动态分配，入队即拷贝） */
	int  role;              /* UNIX_SOCK_ROLE_CLIENT / UNIX_SOCK_ROLE_SERVER */
	int  running;           /* 内部线程运行标志 */
	pthread_t rx_thread;    /* 内部收/发事件线程 */
	char sock_path[108];    /* SERVER 模式：listen 路径，关闭时 unlink */
};

/* 工厂 create 的 params 实参结构 */
struct unix_socket_params {
	const char *sock_path;   /* CLIENT：hub 路径；SERVER：本端口 listen 路径 */
	const char *name;        /* 设备名，如 "hub0" */
	int  role;               /* UNIX_SOCK_ROLE_CLIENT / UNIX_SOCK_ROLE_SERVER */
};

enum {
	UNIX_SOCK_ROLE_CLIENT = 0,   /* 栈进程：socket + connect */
	UNIX_SOCK_ROLE_SERVER = 1,   /* hub 端口：socket + bind + listen + accept */
};

/*
 * 接管已就绪 fd 构造（单测/复用场景）：不建 socket，直接接管 fd（设为 O_NONBLOCK）。
 * fd < 0 时返回 -1。
 */
int  unix_socket_netdev_attach(struct unix_socket_netdev *hp, int fd,
			       const char *name);

/* 释放资源：关闭 fd、unlink server 路径、清空并释放发送队列（malloc/free 对称） */
void unix_socket_netdev_teardown(struct unix_socket_netdev *hp);
/* 工厂 create：malloc 实例 + init，返回基类指针；失败返回 NULL */
struct netdev *unix_socket_netdev_create(const void *params);

/* 工厂 destroy：调 unix_socket_netdev_teardown + free（与 create 对称） */
void unix_socket_netdev_destroy(struct netdev *self);

/* 本后端虚表（非 static，供 netdev_destroy 按 ops 反查类型）；函数实现仍 static */
extern const struct netdev_ops unix_socket_netdev_ops;

#endif /* UNIX_SOCKET_NETDEV_H */
