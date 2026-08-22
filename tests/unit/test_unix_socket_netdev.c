/*
 * test_unix_socket_netdev.c — unix_socket 后端单元测试（check 框架，docs/02-netdev与hub.md §7）。
 * socketpair 模拟链路（一端接 unix_socket fd，一端测试持有），验证线程化事件模型：
 *   收：start 起内部线程 → 对端写入 → 线程 read → rx_handler 回调上送完整帧（回调 free）
 *   发：send 入队不阻塞 → 线程 POLLOUT 驱动写出 → 对端到齐
 *   以及断开/ops 分派/container_of/工厂（create server & client、未知类型、NULL）。
 * 运行：cmake -S tests -B <build> && cmake --build <build> && <build>/unit/test_unix_socket_netdev
 */
#include <check.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "unix_socket_netdev.h"
#include "netdev_factory.h"
#include "container_of.h"

/* 回调记录器：记录收到的帧，验证后 free（契约：回调拥有缓冲所有权） */
static struct rx_rec {
	int called;
	uint8_t buf[NETDEV_BUFSIZE];
	size_t len;
} g_rx;
static pthread_mutex_t g_rx_lock = PTHREAD_MUTEX_INITIALIZER;

static void rx_recorder(struct netdev *self, uint8_t *buf, size_t len)
{
	(void)self;
	pthread_mutex_lock(&g_rx_lock);
	g_rx.called++;
	g_rx.len = len;
	memcpy(g_rx.buf, buf, len);
	pthread_mutex_unlock(&g_rx_lock);
	free(buf);
}

/* 等回调发生（带超时，避免线程未及时处理时测试挂死） */
static void wait_rx(int want_called)
{
	unsigned int spins = 0;
	int cur;

	do {
		pthread_mutex_lock(&g_rx_lock);
		cur = g_rx.called;
		pthread_mutex_unlock(&g_rx_lock);
		if (cur >= want_called)
			break;
		usleep(100);
	} while (spins++ < 1000000);
}

static struct unix_socket_netdev mk_unix_socket_netdev(int *sv)
{
	struct unix_socket_netdev hp;

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	ck_assert_int_eq(unix_socket_netdev_attach(&hp, sv[0], "hub0"), 0);
	netdev_set_rx_handler(&hp.base, rx_recorder);
	memset(&g_rx, 0, sizeof(g_rx));
	return hp;
}

/* 收：start 起线程 → 对端写入 → 回调收到完整帧，rx_bytes 累加 */
START_TEST(test_rx_callback)
{
	int sv[2];
	struct unix_socket_netdev hp = mk_unix_socket_netdev(sv);
	struct netdev *nd = &hp.base;
	const uint8_t pkt[] = "hello-world";

	ck_assert_int_eq(netdev_start(nd), 0);
	ck_assert_int_eq(write(sv[1], pkt, sizeof(pkt) - 1),
			 (ssize_t)(sizeof(pkt) - 1));
	wait_rx(1);
	{
		int called;
		size_t len;
		uint8_t rec_buf[NETDEV_BUFSIZE];

		pthread_mutex_lock(&g_rx_lock);
		called = g_rx.called;
		len = g_rx.len;
		memcpy(rec_buf, g_rx.buf, len);
		pthread_mutex_unlock(&g_rx_lock);

		ck_assert_int_eq(called, 1);
		ck_assert_int_eq(len, sizeof(pkt) - 1);
		ck_assert_int_eq(memcmp(rec_buf, pkt, len), 0);
	}
	ck_assert_int_eq(nd->rx_bytes, sizeof(pkt) - 1);
	netdev_stop(nd);
	unix_socket_netdev_teardown(&hp);
	close(sv[1]);
}
END_TEST

/* 收：连续多帧，每帧回调拿到独立缓冲、内容正确 */
START_TEST(test_rx_independent_buffers)
{
	int sv[2];
	struct unix_socket_netdev hp = mk_unix_socket_netdev(sv);
	struct netdev *nd = &hp.base;
	const uint8_t a[] = "first";
	const uint8_t b[] = "second";

	ck_assert_int_eq(netdev_start(nd), 0);
	ck_assert_int_eq(write(sv[1], a, sizeof(a) - 1), (ssize_t)(sizeof(a) - 1));
	wait_rx(1);
	{
		uint8_t rec_buf[NETDEV_BUFSIZE];
		size_t len;
		int called;

		pthread_mutex_lock(&g_rx_lock);
		len = g_rx.len;
		called = g_rx.called;
		memcpy(rec_buf, g_rx.buf, len);
		pthread_mutex_unlock(&g_rx_lock);

		ck_assert_int_eq(called, 1);
		ck_assert_int_eq(memcmp(rec_buf, a, len), 0);
	}
	ck_assert_int_eq(write(sv[1], b, sizeof(b) - 1), (ssize_t)(sizeof(b) - 1));
	wait_rx(2);
	{
		uint8_t rec_buf[NETDEV_BUFSIZE];
		size_t len;
		int called;

		pthread_mutex_lock(&g_rx_lock);
		called = g_rx.called;
		len = g_rx.len;
		memcpy(rec_buf, g_rx.buf, len);
		pthread_mutex_unlock(&g_rx_lock);

		ck_assert_int_eq(called, 2);
		ck_assert_int_eq(memcmp(rec_buf, b, len), 0);
	}
	netdev_stop(nd);
	unix_socket_netdev_teardown(&hp);
	close(sv[1]);
}
END_TEST

/* 收：对端关闭 → 线程退出，stop 不崩溃 */
START_TEST(test_rx_eof)
{
	int sv[2];
	struct unix_socket_netdev hp = mk_unix_socket_netdev(sv);
	struct netdev *nd = &hp.base;

	ck_assert_int_eq(netdev_start(nd), 0);
	close(sv[1]);
	usleep(50000);
	ck_assert_int_eq(g_rx.called, 0);
	netdev_stop(nd);
	unix_socket_netdev_teardown(&hp);
}
END_TEST

/* 发：send 入队立即返回 len（对端未读也不阻塞），线程写出后对端到齐 */
START_TEST(test_tx_drain)
{
	int sv[2];
	struct unix_socket_netdev hp = mk_unix_socket_netdev(sv);
	struct netdev *nd = &hp.base;
	const uint8_t pkt[] = "tx-frame";
	uint8_t got[64] = { 0 };
	ssize_t n = -1;

	ck_assert_int_eq(netdev_start(nd), 0);
	ck_assert_int_eq(netdev_send(nd, pkt, sizeof(pkt) - 1),
			 (ssize_t)(sizeof(pkt) - 1));
	ck_assert_int_eq(nd->tx_bytes, sizeof(pkt) - 1);
	/* 等线程 POLLOUT 写出 */
	{
		unsigned int spins = 0;
		while (n < 0 && spins++ < 1000000) {
			n = read(sv[1], got, sizeof(got));
			if (n < 0)
				usleep(100);
		}
	}
	ck_assert_int_eq(n, (ssize_t)(sizeof(pkt) - 1));
	ck_assert_int_eq(memcmp(got, pkt, sizeof(pkt) - 1), 0);
	netdev_stop(nd);
	unix_socket_netdev_teardown(&hp);
	close(sv[1]);
}
END_TEST

/* 发：超出帧容量上限的帧被拒绝 */
START_TEST(test_tx_oversize_rejected)
{
	int sv[2];
	struct unix_socket_netdev hp = mk_unix_socket_netdev(sv);
	struct netdev *nd = &hp.base;
	uint8_t big[NETDEV_BUFSIZE + 1] = { 0 };

	ck_assert_int_lt(netdev_send(nd, big, sizeof(big)), 0);
	netdev_stop(nd);
	unix_socket_netdev_teardown(&hp);
	close(sv[1]);
}
END_TEST

/* 继承：基指针 == 内嵌基结构地址；container_of 反推子类 fd */
START_TEST(test_inherit_and_container_of)
{
	int sv[2];
	struct unix_socket_netdev hp = mk_unix_socket_netdev(sv);
	struct netdev *nd = &hp.base;
	struct unix_socket_netdev *back;

	ck_assert_ptr_eq(nd, &hp.base);
	back = container_of(nd, struct unix_socket_netdev, base);
	ck_assert_ptr_eq(back, &hp);
	ck_assert_int_eq(back->fd, sv[0]);
	ck_assert_str_eq(nd->name, "hub0");
	netdev_stop(nd);
	unix_socket_netdev_teardown(&hp);
	close(sv[1]);
}
END_TEST

/* ops 分派：init 校验虚表绑定，多态调用走 unix_socket 实现 */
START_TEST(test_ops_dispatch)
{
	int sv[2];
	struct unix_socket_netdev hp = mk_unix_socket_netdev(sv);
	struct netdev *nd = &hp.base;

	ck_assert_int_eq(netdev_init(nd), 0);
	netdev_stop(nd);        /* 未 start 时 stop 幂等，不应崩溃 */
	unix_socket_netdev_teardown(&hp);
	close(sv[1]);
}
END_TEST

/* 构造失败：无效 fd 拒绝 */
START_TEST(test_construct_fail)
{
	struct unix_socket_netdev hp;

	ck_assert_int_lt(unix_socket_netdev_attach(&hp, -1, "hub0"), 0);
}
END_TEST

/* 工厂：未知类型名 → create 返回 NULL */
START_TEST(test_factory_create_unknown_type)
{
	ck_assert_ptr_null(netdev_create("no_such_backend", NULL));
}
END_TEST

/* 工厂：create NULL params → 返回 NULL；netdev_destroy(NULL) 不崩溃 */
START_TEST(test_factory_null_params)
{
	ck_assert_ptr_null(netdev_create("unix_socket", NULL));
	netdev_destroy(NULL);
}
END_TEST

static Suite *make_suite(void)
{
	Suite *s = suite_create("netdev-unix-socket");
	TCase *tc = tcase_create("unix_socket_netdev");

	tcase_add_test(tc, test_rx_callback);
	tcase_add_test(tc, test_rx_independent_buffers);
	tcase_add_test(tc, test_rx_eof);
	tcase_add_test(tc, test_tx_drain);
	tcase_add_test(tc, test_tx_oversize_rejected);
	tcase_add_test(tc, test_inherit_and_container_of);
	tcase_add_test(tc, test_ops_dispatch);
	tcase_add_test(tc, test_construct_fail);
	tcase_add_test(tc, test_factory_create_unknown_type);
	tcase_add_test(tc, test_factory_null_params);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int nf;
	Suite *s = make_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nf = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nf == 0 ? 0 : 1;
}
