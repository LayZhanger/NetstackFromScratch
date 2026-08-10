/*
 * hubport.c：netdev 的 hub 端口后端（docs/02-netdev与hub.md §4）。
 * 事件驱动模型（01-架构.md §2）：
 *   - 收：poll 检测 POLLIN → 按帧 malloc 缓冲 → read → rx_handler 回调上送（回调 free）。
 *   - 发：send 只入队（malloc 节点+帧缓冲并拷贝，不阻塞）；poll 检测 POLLOUT 驱动写出，
 *     部分写记录 off，EAGAIN 留队头等下个 POLLOUT 自动重试，写完 free 出队。
 * 虚方法实现全部 static，经 hubport_ops 注册进基类虚表（c-style skill §3.2）。
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "hubport.h"
#include "container_of.h"

/* ===== 发送队列：链表，节点与帧缓冲均 malloc（01-架构.md §6 允许动态内存） ===== */

struct txq_entry {
	uint8_t *buf;               /* 帧缓冲（入队时从调用方拷贝） */
	size_t   len;               /* 帧长 */
	size_t   off;               /* 已写出偏移（部分写续写用） */
	struct txq_entry *next;
};

struct txq {
	struct txq_entry *head;     /* 队头：待写出 */
	struct txq_entry *tail;     /* 队尾：新入队 */
};

static struct txq_entry *txq_push(struct txq *q, const uint8_t *buf, size_t len)
{
	struct txq_entry *e = malloc(sizeof(*e));

	if (!e)
		return NULL;
	e->buf = malloc(len);
	if (!e->buf) {
		free(e);
		return NULL;
	}
	memcpy(e->buf, buf, len);
	e->len = len;
	e->off = 0;
	e->next = NULL;
	if (q->tail)
		q->tail->next = e;
	else
		q->head = e;
	q->tail = e;
	return e;
}

static void txq_pop(struct txq *q)
{
	struct txq_entry *e = q->head;

	q->head = e->next;
	if (!q->head)
		q->tail = NULL;
	free(e->buf);
	free(e);
}

static void txq_flush(struct txq *q)
{
	while (q->head)
		txq_pop(q);
}

/* ===== 虚方法实现 ===== */

static const struct netdev_ops hubport_ops;   /* 前置声明：init 中校验分派用 */

static int hubport_init(struct netdev *self)
{
	struct hubport_netdev *hp = container_of(self, struct hubport_netdev, base);

	if (self->ops != &hubport_ops)      /* 校验分派：self 必须是 hubport 实例 */
		return -1;
	if (hp->fd < 0 || !hp->txq)
		return -1;
	return 0;
}

/* 驱动发送队列写出：把队头帧尽量写完，EAGAIN 停等下次 POLLOUT */
static int hubport_tx_drain(struct hubport_netdev *hp)
{
	while (hp->txq->head) {
		struct txq_entry *e = hp->txq->head;
		ssize_t n = write(hp->fd, e->buf + e->off, e->len - e->off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;       /* 写缓冲满：留队头，等下个 POLLOUT */
			return -1;              /* 致命错误（如对端关闭） */
		}
		e->off += (size_t)n;
		if (e->off == e->len)
			txq_pop(hp->txq);
	}
	return 0;
}

static int hubport_poll(struct netdev *self, int timeout_ms)
{
	struct hubport_netdev *hp = container_of(self, struct hubport_netdev, base);
	struct pollfd pfd;
	int ret;

	pfd.fd = hp->fd;
	pfd.events = POLLIN;
	if (hp->txq->head)
		pfd.events |= POLLOUT;
	pfd.revents = 0;

	ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0) {
		if (errno == EINTR)
			return 0;               /* 被信号打断：视同超时 */
		return -1;
	}
	if (ret == 0)
		return 0;                       /* 超时无事件：主循环转周期定时 */
	if (pfd.revents & (POLLERR | POLLNVAL))
		return -1;

	if (pfd.revents & (POLLIN | POLLHUP)) {
		uint8_t *buf = malloc(NETDEV_BUFSIZE);
		ssize_t n;

		if (!buf)
			return -1;
		do {
			n = read(hp->fd, buf, NETDEV_BUFSIZE);
		} while (n < 0 && errno == EINTR);
		if (n <= 0) {
			free(buf);
			return -1;               /* EOF/错误：链路断开 */
		}
		self->rx_bytes += (size_t)n;
		if (self->rx_handler) {
			/* 回调拥有 buf 所有权，处理完必须 free */
			self->rx_handler(self, buf, (size_t)n);
		} else {
			free(buf);
		}
	}
	if (pfd.revents & POLLOUT) {
		if (hubport_tx_drain(hp) < 0)
			return -1;
	}
	return 1;                               /* 有活动 */
}

static int hubport_send(struct netdev *self, const uint8_t *buf, size_t len)
{
	struct hubport_netdev *hp = container_of(self, struct hubport_netdev, base);

	if (len == 0 || len > NETDEV_BUFSIZE)
		return -1;
	if (!txq_push(hp->txq, buf, len))
		return -1;
	self->tx_bytes += len;
	return (int)len;
}

static void hubport_periodic(struct netdev *self)
{
	/* 本阶段无定时业务，预留（后续 ARP 老化/QUIC 重传用，01-架构.md §2） */
	(void)self;
}

static const struct netdev_ops hubport_ops = {
	.init     = hubport_init,
	.poll     = hubport_poll,
	.send     = hubport_send,
	.periodic = hubport_periodic,
};

/* ===== 构造 / 销毁 ===== */

int hubport_netdev_init(struct hubport_netdev *hp, const char *sock_path,
			const char *name)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(sock_path) >= sizeof(addr.sun_path)) {
		close(fd);
		return -1;
	}
	strcpy(addr.sun_path, sock_path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return hubport_netdev_attach(hp, fd, name);
}

int hubport_netdev_attach(struct hubport_netdev *hp, int fd, const char *name)
{
	int flags;

	if (fd < 0)
		return -1;
	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	memset(hp, 0, sizeof(*hp));
	hp->txq = malloc(sizeof(*hp->txq));
	if (!hp->txq)
		return -1;
	hp->txq->head = hp->txq->tail = NULL;
	hp->fd = fd;
	hp->base.ops = &hubport_ops;
	hp->base.name = name;
	return 0;
}

void hubport_netdev_destroy(struct hubport_netdev *hp)
{
	if (hp->fd >= 0)
		close(hp->fd);
	hp->fd = -1;
	if (hp->txq) {
		txq_flush(hp->txq);
		free(hp->txq);
		hp->txq = NULL;
	}
}
