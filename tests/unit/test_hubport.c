/*
 * test_hubport.c — hubport 后端单元测试（check 框架，docs/02-netdev与hub.md §7）。
 * socketpair 模拟链路（一端接 hubport fd，一端测试持有），验证事件驱动模型：
 *   收：poll 检测 POLLIN → 回调上送完整帧（回调持有并 free 缓冲）
 *   发：send 入队不阻塞 → poll 的 POLLOUT 驱动写出 → 对端到齐
 *   以及超时/断开/ops 分派/container_of。
 * 运行：cmake -S tests -B <build> && cmake --build <build> && <build>/unit/test_hubport
 */
#include <check.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hubport.h"
#include "container_of.h"

/* 回调记录器：记录收到的帧，验证后 free（契约：回调拥有缓冲所有权） */
static struct rx_rec {
	int called;
	uint8_t buf[NETDEV_BUFSIZE];
	size_t len;
} g_rx;

static void rx_recorder(struct netdev *self, uint8_t *buf, size_t len)
{
	(void)self;
	g_rx.called++;
	g_rx.len = len;
	memcpy(g_rx.buf, buf, len);
	free(buf);
}

static struct hubport_netdev mk_hubport(int *sv)
{
	struct hubport_netdev hp;

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	ck_assert_int_eq(hubport_netdev_attach(&hp, sv[0], "hub0"), 0);
	netdev_set_rx_handler(&hp.base, rx_recorder);
	memset(&g_rx, 0, sizeof(g_rx));
	return hp;
}

/* 收：对端写入 → poll 返回 1，回调收到完整帧，rx_bytes 累加 */
START_TEST(test_rx_callback)
{
	int sv[2];
	struct hubport_netdev hp = mk_hubport(sv);
	struct netdev *nd = &hp.base;
	const uint8_t pkt[] = "hello-world";

	ck_assert_int_eq(write(sv[1], pkt, sizeof(pkt) - 1),
			 (ssize_t)(sizeof(pkt) - 1));
	ck_assert_int_eq(netdev_poll(nd, 1000), 1);
	ck_assert_int_eq(g_rx.called, 1);
	ck_assert_int_eq(g_rx.len, sizeof(pkt) - 1);
	ck_assert_int_eq(memcmp(g_rx.buf, pkt, sizeof(pkt) - 1), 0);
	ck_assert_int_eq(nd->rx_bytes, sizeof(pkt) - 1);
	hubport_netdev_destroy(&hp);
	close(sv[1]);
}
END_TEST

/* 收：连续多帧，每帧回调拿到独立缓冲、内容正确 */
START_TEST(test_rx_independent_buffers)
{
	int sv[2];
	struct hubport_netdev hp = mk_hubport(sv);
	struct netdev *nd = &hp.base;
	const uint8_t a[] = "first";
	const uint8_t b[] = "second";

	ck_assert_int_eq(write(sv[1], a, sizeof(a) - 1), (ssize_t)(sizeof(a) - 1));
	ck_assert_int_eq(netdev_poll(nd, 1000), 1);
	ck_assert_int_eq(memcmp(g_rx.buf, a, sizeof(a) - 1), 0);
	ck_assert_int_eq(write(sv[1], b, sizeof(b) - 1), (ssize_t)(sizeof(b) - 1));
	ck_assert_int_eq(netdev_poll(nd, 1000), 1);
	ck_assert_int_eq(g_rx.called, 2);
	ck_assert_int_eq(memcmp(g_rx.buf, b, sizeof(b) - 1), 0);
	hubport_netdev_destroy(&hp);
	close(sv[1]);
}
END_TEST

/* 收：无数据超时返回 0（主循环转周期定时） */
START_TEST(test_rx_timeout)
{
	int sv[2];
	struct hubport_netdev hp = mk_hubport(sv);
	struct netdev *nd = &hp.base;

	ck_assert_int_eq(netdev_poll(nd, 50), 0);
	ck_assert_int_eq(g_rx.called, 0);
	hubport_netdev_destroy(&hp);
	close(sv[1]);
}
END_TEST

/* 收：对端关闭 → poll 返回 -1（链路断开） */
START_TEST(test_rx_eof)
{
	int sv[2];
	struct hubport_netdev hp = mk_hubport(sv);
	struct netdev *nd = &hp.base;

	close(sv[1]);
	ck_assert_int_lt(netdev_poll(nd, 1000), 0);
	hubport_netdev_destroy(&hp);
}
END_TEST

/* 发：send 入队立即返回 len（对端未读也不阻塞），poll 驱动写出后对端到齐 */
START_TEST(test_tx_drain)
{
	int sv[2];
	struct hubport_netdev hp = mk_hubport(sv);
	struct netdev *nd = &hp.base;
	const uint8_t pkt[] = "tx-frame";
	uint8_t got[64] = { 0 };
	ssize_t n;

	ck_assert_int_eq(netdev_send(nd, pkt, sizeof(pkt) - 1),
			 (ssize_t)(sizeof(pkt) - 1));
	ck_assert_int_eq(nd->tx_bytes, sizeof(pkt) - 1);
	ck_assert_int_eq(netdev_poll(nd, 1000), 1);   /* POLLOUT 驱动写出 */
	n = read(sv[1], got, sizeof(got));
	ck_assert_int_eq(n, (ssize_t)(sizeof(pkt) - 1));
	ck_assert_int_eq(memcmp(got, pkt, sizeof(pkt) - 1), 0);
	hubport_netdev_destroy(&hp);
	close(sv[1]);
}
END_TEST

/* 发：多帧连续入队，poll 逐帧写出，顺序与内容到齐（覆盖队列与部分写续写路径） */
START_TEST(test_tx_multiple_frames)
{
	int sv[2];
	struct hubport_netdev hp = mk_hubport(sv);
	struct netdev *nd = &hp.base;
	static const char *frames[] = { "frame-1", "frame-22", "frame-333" };
	uint8_t got[NETDEV_BUFSIZE] = { 0 };
	size_t got_len = 0, i;
	ssize_t n;

	for (i = 0; i < 3; i++)
		ck_assert_int_eq(netdev_send(nd, (const uint8_t *)frames[i],
					     strlen(frames[i])),
				 (ssize_t)strlen(frames[i]));
	while (netdev_poll(nd, 100) > 0)
		;
	n = read(sv[1], got + got_len, sizeof(got) - got_len);
	ck_assert_int_ge(n, 0);
	got_len += (size_t)n;
	ck_assert_int_eq(got_len, 7 + 8 + 9);   /* 3 帧全部到齐 */

	/* 字节流无边界，逐帧核对顺序 */
	i = 0;
	ck_assert_int_eq(memcmp(got + i, frames[0], 7), 0);   i += 7;
	ck_assert_int_eq(memcmp(got + i, frames[1], 8), 0);   i += 8;
	ck_assert_int_eq(memcmp(got + i, frames[2], 9), 0);   i += 9;
	ck_assert_int_eq(i, 7 + 8 + 9);
	hubport_netdev_destroy(&hp);
	close(sv[1]);
}
END_TEST

/* 发：超出帧容量上限的帧被拒绝 */
START_TEST(test_tx_oversize_rejected)
{
	int sv[2];
	struct hubport_netdev hp = mk_hubport(sv);
	struct netdev *nd = &hp.base;
	uint8_t big[NETDEV_BUFSIZE + 1] = { 0 };

	ck_assert_int_lt(netdev_send(nd, big, sizeof(big)), 0);
	hubport_netdev_destroy(&hp);
	close(sv[1]);
}
END_TEST

/* 继承：基指针 == 内嵌基结构地址；container_of 反推子类 fd */
START_TEST(test_inherit_and_container_of)
{
	int sv[2];
	struct hubport_netdev hp = mk_hubport(sv);
	struct netdev *nd = &hp.base;
	struct hubport_netdev *back;

	ck_assert_ptr_eq(nd, &hp.base);
	back = container_of(nd, struct hubport_netdev, base);
	ck_assert_ptr_eq(back, &hp);
	ck_assert_int_eq(back->fd, sv[0]);
	ck_assert_str_eq(nd->name, "hub0");
	hubport_netdev_destroy(&hp);
	close(sv[1]);
}
END_TEST

/* ops 分派：init 校验虚表绑定，多态调用走 hubport 实现 */
START_TEST(test_ops_dispatch)
{
	int sv[2];
	struct hubport_netdev hp = mk_hubport(sv);
	struct netdev *nd = &hp.base;

	ck_assert_int_eq(netdev_init(nd), 0);
	nd->ops->periodic(nd);      /* 空实现，不应崩溃 */
	hubport_netdev_destroy(&hp);
	close(sv[1]);
}
END_TEST

/* 构造失败：无效 fd 拒绝；坏 socket 路径 init 失败 */
START_TEST(test_construct_fail)
{
	struct hubport_netdev hp;

	ck_assert_int_lt(hubport_netdev_attach(&hp, -1, "hub0"), 0);
	ck_assert_int_lt(hubport_netdev_init(&hp, "/tmp/nonexistent.sock",
					     "hub0"), 0);
}
END_TEST

static Suite *make_suite(void)
{
	Suite *s = suite_create("netdev-hubport");
	TCase *tc = tcase_create("hubport");

	tcase_add_test(tc, test_rx_callback);
	tcase_add_test(tc, test_rx_independent_buffers);
	tcase_add_test(tc, test_rx_timeout);
	tcase_add_test(tc, test_rx_eof);
	tcase_add_test(tc, test_tx_drain);
	tcase_add_test(tc, test_tx_multiple_frames);
	tcase_add_test(tc, test_tx_oversize_rejected);
	tcase_add_test(tc, test_inherit_and_container_of);
	tcase_add_test(tc, test_ops_dispatch);
	tcase_add_test(tc, test_construct_fail);
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
