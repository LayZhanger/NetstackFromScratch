#ifndef HUB_H
#define HUB_H

#include <stddef.h>
#include <stdint.h>

#include "netdev.h"

/*
 * hub：虚拟交换机核心（01-架构.md §4）。
 * 复用 netdev 作为端口：每个端口 = 一个 netdev 实例（"unix_socket" 后端 SERVER 角色），
 * fd/accept/线程全部在 netdev 内部，hub 不写任何 socket 代码。
 * 转发规则（本阶段简化）：收到某端口一帧 → 转发到其它所有端口（共享介质语义）。
 * 广播/组播/单播细分留到 eth 阶段（01-架构.md §4 转发规则表）。
 * 启动固定配置：hub_init 创建并启动 N 个端口 netdev，之后线程各自驱动收发。
 */

#define HUB_MAX_PORTS 8         /* 端口表静态数组，编译期定（教学默认简单优先） */
#define HUB_SOCK_PATH_SIZE 108   /* sun_path 上限 */

struct hub {
	struct netdev *ports[HUB_MAX_PORTS];      /* 端口 = netdev 实例（SERVER 角色） */
	int  nports;                              /* 已配置端口数（固定） */
	int  loss[HUB_MAX_PORTS];                 /* 各端口丢包率百分比 0-100 */
	unsigned long dropped[HUB_MAX_PORTS];     /* 各端口因丢包丢弃的帧数 */
	char sock_base[HUB_SOCK_PATH_SIZE];       /* 端口 socket 路径前缀（<base>-<i>.sock） */
};

/*
 * 初始化并启动：给每个端口创建 server-role netdev（路径 <sock_base>-<i>.sock）。
 * nports：固定端口数；loss_by_port：各端口丢包率（可 NULL）；nloss：数组长度。
 * 成功返回 0，失败返回 -1（部分创建的端口会被清理）。nports 不得 > HUB_MAX_PORTS。
 */
int  hub_init(struct hub *self, const char *sock_base, int nports,
	      const int *loss_by_port, int nloss);

/* 阻塞运行：端口线程各自驱动收发，转发在 rx_handler 中完成；本函数仅等待 */
void hub_run(struct hub *self);

/* 清理全部端口（netdev_destroy）、unlink socket 路径 */
void hub_destroy(struct hub *self);

#endif /* HUB_H */
