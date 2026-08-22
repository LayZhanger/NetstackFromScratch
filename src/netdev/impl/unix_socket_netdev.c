/*
 * unix_socket_netdev.c：netdev 的 unix socket 后端（docs/02-netdev与hub.md §4）。
 * 运行时（01-架构.md §2 线程化版）：内部起一个事件线程——
 *   - 收：poll 检测 POLLIN → 按帧 malloc 缓冲 → read → rx_handler 回调上送（回调 free）。
 *   - 发：send 只入队（malloc 节点+帧缓冲并拷贝，不阻塞）；线程 poll 检测 POLLOUT 驱动写出，
 *     部分写记录 off，EAGAIN 留队头等下个 POLLOUT 自动重试，写完 free 出队。
 * 虚方法实现全部 static，经 unix_socket_netdev_ops 注册进基类虚表（c-style skill §3.2）。
 * txq 受 pthread_mutex_t 保护（send 可来自多个线程：hub 转发各端口线程并发 send）。
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "unix_socket_netdev.h"
#include "container_of.h"

/* ===== 发送队列：链表，节点与帧缓冲均 malloc（01-架构.md §6 允许动态内存） ===== */

struct txq_entry {
	uint8_t *buf;               /* 帧缓冲（入队时从调用方拷贝） */
	size_t   len;               /* 帧长 */
	size_t   off;               /* 已写出偏移（部分写续写用） */
	struct txq_entry *next;
};

struct txq {
	pthread_mutex_t lock;       /* 保护 head/tail（send 多线程并发） */
	struct txq_entry *head;     /* 队头：待写出 */
	struct txq_entry *tail;     /* 队尾：新入队 */
};

static struct txq_entry *txq_take(struct txq *q)
{
	struct txq_entry *e;

	pthread_mutex_lock(&q->lock);
	e = q->head;
	if (e) {
		q->head = e->next;
		if (!q->head)
			q->tail = NULL;
	}
	pthread_mutex_unlock(&q->lock);
	return e;               /* 所有权移交调用方，调用方负责 free */
}

/* 驱动发送队列写出：把队头帧尽量写完，EAGAIN 停等下次 POLLOUT */
static int unix_socket_tx_drain(struct unix_socket_netdev *hp)
{
	for (;;) {
		struct txq_entry *e = txq_take(hp->txq);
		ssize_t n;

		if (!e)
			return 0;
		n = write(hp->fd, e->buf + e->off, e->len - e->off);
		if (n < 0) {
			free(e->buf);
			free(e);
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;       /* 写缓冲满：已出队丢弃，下帧 POLLOUT 再写 */
			return -1;              /* 致命错误（如对端关闭） */
		}
		e->off += (size_t)n;
		if (e->off < e->len) {
			/* 部分写：剩下补回队头，等下次 POLLOUT 续写 */
			pthread_mutex_lock(&hp->txq->lock);
			e->next = hp->txq->head;
			hp->txq->head = e;
			if (!hp->txq->tail)
				hp->txq->tail = e;
			pthread_mutex_unlock(&hp->txq->lock);
			return 0;
		}
		free(e->buf);
		free(e);
	}
}

/* ===== 内部事件线程（收 + 发，均非阻塞） ===== */

static void *unix_socket_thread(void *arg)
{
	struct unix_socket_netdev *hp = arg;
	struct netdev *self = &hp->base;

	while (hp->running) {
		struct pollfd pfd;
		int ret;

		pfd.fd = hp->fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		/* txq 有待发帧时才轮询 POLLOUT（读取前上锁） */
		pthread_mutex_lock(&hp->txq->lock);
		if (hp->txq->head)
			pfd.events |= POLLOUT;
		pthread_mutex_unlock(&hp->txq->lock);
		ret = poll(&pfd, 1, 100);            /* 超时 100ms：维持 running 检查 */
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;                          /* 致命错误：线程退出 */
		}
		if (ret == 0)
			continue;                       /* 超时：检查 running */
		if (pfd.revents & (POLLERR | POLLNVAL))
			break;
		if (pfd.revents & (POLLIN | POLLHUP)) {
			uint8_t *buf = malloc(NETDEV_BUFSIZE);
			ssize_t n;

			if (!buf)
				break;
			do {
				n = read(hp->fd, buf, NETDEV_BUFSIZE);
			} while (n < 0 && errno == EINTR);
			if (n <= 0) {
				free(buf);
				break;                  /* EOF/错误：对端关闭，线程退出 */
			}
			self->rx_bytes += (size_t)n;
			if (self->rx_handler) {
				/* 回调拥有 buf 所有权，处理完必须 free（线程上下文） */
				self->rx_handler(self, buf, (size_t)n);
			} else {
				free(buf);
			}
		}
		if (pfd.revents & POLLOUT) {
			if (unix_socket_tx_drain(hp) < 0)
				break;
		}
	}
	return NULL;
}

/* ===== 虚方法实现 ===== */

const struct netdev_ops unix_socket_netdev_ops;   /* 前置声明：init 中校验分派用 */

static int unix_socket_init(struct netdev *self)
{
	struct unix_socket_netdev *hp = container_of(self, struct unix_socket_netdev, base);

	if (self->ops != &unix_socket_netdev_ops)      /* 校验分派：self 必须是 unix_socket 实例 */
		return -1;
	if (hp->fd < 0 || !hp->txq)
		return -1;
	return 0;
}

static int unix_socket_start(struct netdev *self)
{
	struct unix_socket_netdev *hp = container_of(self, struct unix_socket_netdev, base);

	if (hp->running)
		return 0;
	hp->running = 1;
	if (pthread_create(&hp->rx_thread, NULL, unix_socket_thread, hp) != 0) {
		hp->running = 0;
		return -1;
	}
	return 0;
}

static int unix_socket_send(struct netdev *self, const uint8_t *buf, size_t len)
{
	struct unix_socket_netdev *hp = container_of(self, struct unix_socket_netdev, base);
	struct txq_entry *e;

	if (len == 0 || len > NETDEV_BUFSIZE)
		return -1;
	e = malloc(sizeof(*e));
	if (!e)
		return -1;
	e->buf = malloc(len);
	if (!e->buf) {
		free(e);
		return -1;
	}
	memcpy(e->buf, buf, len);
	e->len = len;
	e->off = 0;
	e->next = NULL;

	pthread_mutex_lock(&hp->txq->lock);
	if (hp->txq->tail)
		hp->txq->tail->next = e;
	else
		hp->txq->head = e;
	hp->txq->tail = e;
	pthread_mutex_unlock(&hp->txq->lock);

	self->tx_bytes += len;
	return (int)len;
}

static void unix_socket_stop(struct netdev *self)
{
	struct unix_socket_netdev *hp = container_of(self, struct unix_socket_netdev, base);

	hp->running = 0;
	if (hp->rx_thread != 0)
		pthread_join(hp->rx_thread, NULL);
	hp->rx_thread = 0;
}

const struct netdev_ops unix_socket_netdev_ops = {
	.init     = unix_socket_init,
	.start    = unix_socket_start,
	.send     = unix_socket_send,
	.stop     = unix_socket_stop,
};

/* ===== 构造 / 销毁 ===== */

int unix_socket_netdev_attach(struct unix_socket_netdev *hp, int fd,
			      const char *name)
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
	pthread_mutex_init(&hp->txq->lock, NULL);
	hp->txq->head = hp->txq->tail = NULL;
	hp->fd = fd;
	hp->base.ops = &unix_socket_netdev_ops;
	hp->base.name = name;
	return 0;
}

void unix_socket_netdev_teardown(struct unix_socket_netdev *hp)
{
	if (hp->fd >= 0)
		close(hp->fd);
	hp->fd = -1;
	if (hp->sock_path[0] != '\0')
		unlink(hp->sock_path);
	if (hp->txq) {
		struct txq_entry *e;

		/* 停线程后清空残留队列（stop 已 join） */
		while ((e = txq_take(hp->txq)) != NULL) {
			free(e->buf);
			free(e);
		}
		pthread_mutex_destroy(&hp->txq->lock);
		free(hp->txq);
		hp->txq = NULL;
	}
}

/* 工厂 create：按 role 建 fd */
struct netdev *unix_socket_netdev_create(const void *params)
{
	const struct unix_socket_params *p = params;
	struct unix_socket_netdev *hp;

	if (!p || !p->name)
		return NULL;
	hp = malloc(sizeof(*hp));
	if (!hp)
		return NULL;
	memset(hp, 0, sizeof(*hp));

	if (p->role == UNIX_SOCK_ROLE_SERVER) {
		struct sockaddr_un addr;
		int fd;

		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			goto fail;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		if (strlen(p->sock_path) >= sizeof(addr.sun_path)) {
			close(fd);
			goto fail;
		}
		strcpy(addr.sun_path, p->sock_path);
		unlink(p->sock_path);
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			close(fd);
			goto fail;
		}
		if (listen(fd, 8) < 0) {
			close(fd);
			goto fail;
		}
		/* accept 一个连接（SERVER 角色：每个端口恰好一个对端） */
		{
			struct sockaddr_un cl_addr;
			socklen_t cl_len = sizeof(cl_addr);
			int cfd = accept(fd, (struct sockaddr *)&cl_addr, &cl_len);

			close(fd);
			if (cfd < 0)
				goto fail;
			if (unix_socket_netdev_attach(hp, cfd, p->name) < 0) {
				close(cfd);
				goto fail;
			}
		}
		strncpy(hp->sock_path, p->sock_path, sizeof(hp->sock_path) - 1);
		hp->sock_path[sizeof(hp->sock_path) - 1] = '\0';
	} else {
		struct sockaddr_un addr;
		int fd;

		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			goto fail;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		if (strlen(p->sock_path) >= sizeof(addr.sun_path)) {
			close(fd);
			goto fail;
		}
		strcpy(addr.sun_path, p->sock_path);
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			close(fd);
			goto fail;
		}
		if (unix_socket_netdev_attach(hp, fd, p->name) < 0) {
			close(fd);
			goto fail;
		}
	}
	hp->role = p->role;
	return &hp->base;

fail:
	free(hp);
	return NULL;
}

void unix_socket_netdev_destroy(struct netdev *self)
{
	struct unix_socket_netdev *hp;

	if (!self)
		return;
	hp = container_of(self, struct unix_socket_netdev, base);
	/* 若线程未停则先停（stop 幂等） */
	hp->running = 0;
	if (hp->rx_thread != 0)
		pthread_join(hp->rx_thread, NULL);
	unix_socket_netdev_teardown(hp);
	free(hp);
}
