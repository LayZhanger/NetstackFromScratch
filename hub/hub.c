/*
 * hub.c：虚拟交换机（docs/02-netdev与hub.md §5，01-架构.md §4 简化版）。
 * 独立进程：listen unix socket，连接即端口注册；select 单线程驱动，无并发。
 * 转发规则（阶段零简化）：收到某端口一帧 → 原样写往其它所有端口（共享介质语义）。
 * 广播/组播/单播细分留到 eth 阶段（01-架构.md §4 转发规则表）。
 * 用法：hub <socket_path> [丢包率0% 丢包率1% ...]   （按连接注册顺序对应端口）
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define HUB_MAX_PORTS 8         /* 端口表静态数组，编译期定（教学默认简单优先） */
#define HUB_BUFSIZE 2048

struct hub_port {
	int  fd;                    /* -1 = 空闲 */
	int  loss;                  /* 丢包率百分比 0-100（01-架构.md §4 确定性测试） */
	unsigned long rx_frames;    /* 收到帧数 */
	unsigned long tx_frames;    /* 转发帧数 */
	unsigned long dropped;      /* 因丢包率丢弃帧数 */
};

static struct hub_port ports[HUB_MAX_PORTS];
static int listen_fd = -1;

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

static int port_add(int fd)
{
	int i;

	for (i = 0; i < HUB_MAX_PORTS; i++) {
		if (ports[i].fd < 0) {
			ports[i].fd = fd;
			printf("== 端口%d 上线 ==\n", i);
			return i;
		}
	}
	fprintf(stderr, "hub: 端口已满 (%d)\n", HUB_MAX_PORTS);
	return -1;
}

static void port_remove(int idx)
{
	printf("== 端口%d 下线 ==\n", idx);
	close(ports[idx].fd);
	ports[idx].fd = -1;
	ports[idx].loss = 0;
}

/* 按端口丢包率决定是否丢弃（确定性测试用） */
static int should_drop(int idx)
{
	struct hub_port *p = &ports[idx];

	if (p->loss <= 0)
		return 0;
	return (rand() % 100) < p->loss;
}

/* 收到 src 端口一帧：转发到其它所有端口（01-架构.md §4 广播语义的简化） */
static void hub_forward(int src, const uint8_t *buf, size_t len)
{
	int dst;

	ports[src].rx_frames++;
	for (dst = 0; dst < HUB_MAX_PORTS; dst++) {
		size_t written = 0;

		if (dst == src || ports[dst].fd < 0)
			continue;
		if (should_drop(dst)) {
			ports[dst].dropped++;
			printf("[丢包] 端口%d -> 端口%d len=%zu\n", src, dst, len);
			continue;
		}
		while (written < len) {
			ssize_t n = write(ports[dst].fd, buf + written, len - written);

			if (n < 0) {
				if (errno == EINTR)
					continue;
				port_remove(dst);
				break;
			}
			written += (size_t)n;
		}
		if (written == len) {
			ports[dst].tx_frames++;
			log_frame("->", src, dst, buf, len);
		}
	}
}

static void hub_handle_rx(int idx)
{
	uint8_t buf[HUB_BUFSIZE];
	ssize_t n = read(ports[idx].fd, buf, sizeof(buf));

	if (n < 0) {
		if (errno == EINTR)
			return;
		port_remove(idx);
		return;
	}
	if (n == 0) {           /* 对端关闭 */
		port_remove(idx);
		return;
	}
	/* 阶段零简化：一次 read 的一块 = 一帧（docs/02-netdev与hub.md §4） */
	hub_forward(idx, buf, (size_t)n);
}

int main(int argc, char *argv[])
{
	struct sockaddr_un addr;
	fd_set rfds;
	int i, maxfd;

	setvbuf(stdout, NULL, _IONBF, 0);   /* 日志重定向时也要实时可见 */
	if (argc < 2) {
		fprintf(stderr, "用法: %s <socket_path> [丢包率0%% 丢包率1%% ...]\n",
			argv[0]);
		return 1;
	}
	srand((unsigned)time(NULL));

	for (i = 0; i < HUB_MAX_PORTS; i++)
		ports[i].fd = -1;

	unlink(argv[1]);
	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(argv[1]) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "hub: socket 路径过长\n");
		return 1;
	}
	strcpy(addr.sun_path, argv[1]);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}
	if (listen(listen_fd, 8) < 0) {
		perror("listen");
		return 1;
	}
	printf("== hub 启动: %s（端口上限 %d）==\n", argv[1], HUB_MAX_PORTS);

	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(listen_fd, &rfds);
		maxfd = listen_fd;
		for (i = 0; i < HUB_MAX_PORTS; i++) {
			if (ports[i].fd >= 0) {
				FD_SET(ports[i].fd, &rfds);
				if (ports[i].fd > maxfd)
					maxfd = ports[i].fd;
			}
		}
		if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			break;
		}
		if (FD_ISSET(listen_fd, &rfds)) {
			int fd = accept(listen_fd, NULL, NULL);
			int idx;

			if (fd < 0) {
				perror("accept");
				continue;
			}
			idx = port_add(fd);
			if (idx < 0) {
				close(fd);
				continue;
			}
			/* 丢包率参数按连接注册顺序依次对应端口 0,1,2... */
			if (argc > idx + 2) {
				ports[idx].loss = atoi(argv[idx + 2]);
				if (ports[idx].loss > 100)
					ports[idx].loss = 100;
				if (ports[idx].loss > 0)
					printf("== 端口%d 丢包率 %d%% ==\n", idx,
					       ports[idx].loss);
			}
		}
		for (i = 0; i < HUB_MAX_PORTS; i++) {
			if (ports[i].fd >= 0 && FD_ISSET(ports[i].fd, &rfds))
				hub_handle_rx(i);
		}
	}
	return 0;
}
