/*
 * demo.c：阶段零演示进程（docs/02-netdev与hub.md §6）。
 * 经 netdev（hubport 后端）连 hub，定时向对端发送本机内容。
 * 事件驱动（01-架构.md §2）：收帧经 rx_handler 回调处理，主循环只做
 * poll 等待 + 周期定时器（超时则发送一帧），收发互不干扰。
 * 用法：demo <hub_socket_path> <名字> [发送内容]
 */

#define _POSIX_C_SOURCE 200809L   /* c99 严格模式：clock_gettime 需 POSIX 宏 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hubport.h"

#define DEMO_INTERVAL_MS 1000   /* 周期发送间隔 */

static struct hubport_netdev g_hp;
static const char *g_name;

/* 收帧回调：打印收到的字节；buf 为按帧分配的缓冲，处理完必须 free */
static void demo_rx_handler(struct netdev *self, uint8_t *buf, size_t len)
{
	(void)self;
	printf("[%s] 收到 %zu 字节: \"%.*s\"\n", g_name, len, (int)len, buf);
	free(buf);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char *argv[])
{
	const char *sock_path, *payload;
	struct netdev *nd;
	long tx;
	const char *default_payload[] = { "hello-from-A", "hello-from-B" };

	setvbuf(stdout, NULL, _IONBF, 0);   /* 日志重定向时也要实时可见 */
	if (argc < 3) {
		fprintf(stderr, "用法: %s <hub_socket_path> <名字A|B> [发送内容]\n",
			argv[0]);
		return 1;
	}
	sock_path = argv[1];
	g_name = argv[2];
	if (argc > 3)
		payload = argv[3];
	else
		payload = default_payload[g_name[0] == 'A' ? 0 : 1];

	if (hubport_netdev_init(&g_hp, sock_path, g_name) < 0) {
		fprintf(stderr, "[%s] 连接 hub %s 失败\n", g_name, sock_path);
		return 1;
	}
	nd = &g_hp.base;
	netdev_set_rx_handler(nd, demo_rx_handler);
	tx = now_ms();
	printf("[%s] 已连接 hub，周期发送: \"%s\"\n", g_name, payload);

	for (;;) {
		long remain = tx - now_ms();
		int ret;

		if (remain <= 0)
			remain = 0;
		ret = netdev_poll(nd, (int)remain);

		if (ret < 0) {                  /* 链路断开/致命错误 */
			printf("[%s] hub 已断开\n", g_name);
			break;
		}
		if (ret == 0 && now_ms() >= tx) {
			/* 周期定时器：绝对时间到达即发送一帧（01-架构.md §2） */
			if (netdev_send(nd, (const uint8_t *)payload,
					strlen(payload)) < 0) {
				perror("netdev_send");
				break;
			}
			printf("[%s] 已发送: \"%s\"\n", g_name, payload);
			tx += DEMO_INTERVAL_MS;
		}
	}
	hubport_netdev_destroy(&g_hp);
	return 0;
}
