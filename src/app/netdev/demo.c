/*
 * demo.c：阶段零演示进程（docs/02-netdev与hub.md §6）。
 * 经 netdev（unix_socket 后端，CLIENT 角色）连 hub 某端口，定时向对端发送本机内容。
 * 运行时（01-架构.md §2 线程化版）：netdev 内部起收帧线程，收帧经 rx_handler 回调处理，
 * 主循环只做 sleep 保活 + 周期发送（netdev_send 入队，由内部线程 POLLOUT 写出）。
 * 用法：demo <hub_port_socket_path> <名字> [发送内容]
 */

#define _POSIX_C_SOURCE 200809L   /* c99 严格模式：clock_gettime 需 POSIX 宏 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "netdev.h"
#include "netdev_factory.h"
#include "unix_socket_netdev.h"   /* 仅取 unix_socket_params + ROLE 枚举 */

#define DEMO_INTERVAL_MS 1000   /* 周期发送间隔 */

static const char *g_name;

/* 收帧回调：打印收到的字节；buf 为按帧分配的缓冲，处理完必须 free（线程上下文） */
static void demo_rx_handler(struct netdev *self, uint8_t *buf, size_t len)
{
	(void)self;
	printf("[%s] 收到 %zu 字节: \"%.*s\"\n", g_name, len, (int)len, buf);
	free(buf);
}

int main(int argc, char *argv[])
{
	const char *sock_path, *payload;
	struct unix_socket_params p;
	struct netdev *nd;
	const char *default_payload[] = { "hello-from-A", "hello-from-B" };

	setvbuf(stdout, NULL, _IONBF, 0);   /* 日志重定向时也要实时可见 */
	if (argc < 3) {
		fprintf(stderr, "用法: %s <hub_port_socket_path> <名字A|B> [发送内容]\n",
			argv[0]);
		return 1;
	}
	sock_path = argv[1];
	g_name = argv[2];
	if (argc > 3)
		payload = argv[3];
	else
		payload = default_payload[g_name[0] == 'A' ? 0 : 1];

	p.sock_path = sock_path;
	p.name = g_name;
	p.role = UNIX_SOCK_ROLE_CLIENT;
	nd = netdev_create("unix_socket", &p);
	if (!nd) {
		fprintf(stderr, "[%s] 连接 hub 端口 %s 失败\n", g_name, sock_path);
		return 1;
	}
	netdev_set_rx_handler(nd, demo_rx_handler);
	if (netdev_start(nd) < 0) {
		fprintf(stderr, "[%s] 启动收帧线程失败\n", g_name);
		netdev_destroy(nd);
		return 1;
	}
	printf("[%s] 已连接 hub，周期发送: \"%s\"\n", g_name, payload);

	for (;;) {
		sleep(DEMO_INTERVAL_MS / 1000);
		/* 周期发送：入队非阻塞，由内部线程 POLLOUT 写出 */
		if (netdev_send(nd, (const uint8_t *)payload,
				strlen(payload)) < 0) {
			perror("netdev_send");
			break;
		}
		printf("[%s] 已发送: \"%s\"\n", g_name, payload);
	}
	netdev_destroy(nd);
	return 0;
}
