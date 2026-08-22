/*
 * hub.c：虚拟交换机核心（01-架构.md §4）。
 * 复用 netdev：每个端口 = 一个 netdev（unix_socket 后端 SERVER 角色），
 * socket/accept/线程全在 netdev 内部，本文件不写任何 socket 代码。
 * 转发 = 端口 netdev 的 rx_handler：收到一帧 → 转发到其它所有端口（共享介质语义）。
 * 本阶段简化转发（广播语义）：对每个其它端口 netdev_send；单播/组播细分留到 eth 阶段。
 * 启动固定配置：hub_init 创建并启动 N 个端口 netdev，之后由各自内部线程驱动。
 * 用法：hub <socket_base> <nports> [丢包率0% 丢包率1% ...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "hub.h"
#include "netdev_factory.h"
#include "impl/unix_socket_netdev.h"

static struct hub *g_hub;   /* 单实例进程：供 rx_handler 定位 hub 与源端口 */

static void hexdump(const uint8_t *buf, size_t len)
{
	size_t i, show = len < 64 ? len : 64;

	for (i = 0; i < show; i++)
		printf("%02x ", buf[i]);
	if (len > show)
		printf("... ");
	printf("(%zu 字节)\n", len);
}

static void log_frame(const char *dir, int src, int dst, const uint8_t *buf,
		      size_t len)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	printf("[%ld.%03ld] 端口%d %s 端口%d len=%zu  ",
	       (long)tv.tv_sec, (long)tv.tv_usec / 1000, src, dir, dst, len);
	hexdump(buf, len);
}

/* 端口收到一帧：转发到其它所有端口（01-架构.md §4 广播语义的简化） */
static void hub_rx_handler(struct netdev *self, uint8_t *buf, size_t len)
{
	struct hub *hub = g_hub;
	int src = -1, dst, i;

	for (i = 0; i < hub->nports; i++) {
		if (hub->ports[i] == self) {
			src = i;
			break;
		}
	}
	if (src < 0) {              /* 未知端口：按 netdev 契约释放 buf */
		free(buf);
		return;
	}
	for (dst = 0; dst < hub->nports; dst++) {
		if (dst == src)
			continue;
		if (hub->loss[dst] > 0 &&
		    (rand() % 100) < hub->loss[dst]) {
			hub->dropped[dst]++;
			printf("[丢包] 端口%d -> 端口%d len=%zu\n", src, dst, len);
			continue;
		}
		if (netdev_send(hub->ports[dst], buf, len) < 0) {
			printf("端口%d -> 端口%d 发送失败\n", src, dst);
			continue;
		}
		log_frame("->", src, dst, buf, len);
	}
	free(buf);
}

int hub_init(struct hub *self, const char *sock_base, int nports,
	     const int *loss_by_port, int nloss)
{
	int i;

	if (!self || !sock_base || nports <= 0 || nports > HUB_MAX_PORTS)
		return -1;
	memset(self, 0, sizeof(*self));
	if (strlen(sock_base) >= HUB_SOCK_PATH_SIZE)
		return -1;
	self->nports = nports;
	strncpy(self->sock_base, sock_base, HUB_SOCK_PATH_SIZE - 1);
	self->sock_base[HUB_SOCK_PATH_SIZE - 1] = '\0';

	/* 固定配置：每端口一个 server-role netdev，路径 <base>-<i>.sock */
	for (i = 0; i < nports; i++) {
		struct unix_socket_params p;
		char path[HUB_SOCK_PATH_SIZE];
		struct netdev *nd;

		snprintf(path, sizeof(path), "%s-%d.sock", sock_base, i);
		p.sock_path = path;
		p.name = path;
		p.role = UNIX_SOCK_ROLE_SERVER;
		nd = netdev_create("unix_socket", &p);
		if (!nd) {
			perror("netdev_create");
			goto fail;
		}
		if (i < nloss)
			self->loss[i] = loss_by_port[i];
		netdev_set_rx_handler(nd, hub_rx_handler);
		if (netdev_start(nd) < 0) {
			netdev_destroy(nd);
			goto fail;
		}
		self->ports[i] = nd;
	}
	g_hub = self;
	return 0;

fail:
	for (i = 0; i < self->nports; i++)
		if (self->ports[i])
			netdev_destroy(self->ports[i]);
	memset(self->ports, 0, sizeof(self->ports));
	return -1;
}

void hub_run(struct hub *self)
{
	(void)self;
	printf("== hub 启动: %s（端口 %d 个）==\n", self->sock_base,
	       self->nports);
	/* 端口 netdev 各自内部线程已驱动收发（start 时已启动）；
	 * hub 主循环无需 select/accept，仅阻塞等待（线程驱动一切）。 */
	for (;;)
		pause();
}

void hub_destroy(struct hub *self)
{
	int i;

	for (i = 0; i < self->nports; i++) {
		if (self->ports[i]) {
			netdev_destroy(self->ports[i]);
			self->ports[i] = NULL;
		}
	}
}

int main(int argc, char *argv[])
{
	struct hub hub;
	int loss[HUB_MAX_PORTS];
	int nloss = 0, nports = 0, i;

	setvbuf(stdout, NULL, _IONBF, 0);   /* 日志重定向时也要实时可见 */
	if (argc < 3) {
		fprintf(stderr, "用法: %s <socket_base> <nports> [丢包率0%% ...]\n",
			argv[0]);
		return 1;
	}
	srand((unsigned)time(NULL));

	nports = atoi(argv[2]);
	if (nports <= 0 || nports > HUB_MAX_PORTS) {
		fprintf(stderr, "hub: 端口数须在 1-%d 之间\n", HUB_MAX_PORTS);
		return 1;
	}
	for (i = 3; i < argc && nloss < nports; i++) {
		loss[nloss] = atoi(argv[i]);
		if (loss[nloss] > 100)
			loss[nloss] = 100;
		nloss++;
	}

	if (hub_init(&hub, argv[1], nports, loss, nloss) < 0) {
		fprintf(stderr, "hub: 初始化失败\n");
		return 1;
	}
	hub_run(&hub);
	hub_destroy(&hub);
	return 0;
}
