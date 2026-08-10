/*
 * test_ops.c — c-style skill 面向对象机制的单元测试（check 框架）。
 * 验证：基类+ops 表、结构体嵌套继承、多态分派、self 首参、container_of、无 malloc 初始化。
 * 运行：cmake -S tests -B <build> && cmake --build <build> && <build>/test_ops
 */
#include <check.h>
#include <stddef.h>
#include <string.h>

#include "container_of.h"

/* ===== 基类：仿 src/netdev/netdev.h 形态 ===== */

struct netdev;
struct netdev_ops {
    int      (*init)(struct netdev *self);
    ssize_t  (*read)(struct netdev *self, uint8_t *buf, size_t buflen);
    int      (*send)(struct netdev *self, const uint8_t *buf, size_t len);
    void     (*periodic)(struct netdev *self);
};

struct netdev {
    const struct netdev_ops *ops;   /* 虚表，约定放首成员 */
    const char *name;
    size_t   rx_bytes;              /* 收到的总字节数 */
};

/* ===== 子类 1：hub 端口（继承 + 私有数据） ===== */

static const struct netdev_ops hubport_ops;   /* 前置声明：init 中校验分派用 */
static const struct netdev_ops loopback_ops;

struct hubport_netdev {
    struct netdev base;             /* 继承：首成员内嵌基结构 */
    int fd;                         /* 子类私有数据 */
};

static int hubport_init(struct netdev *self)
{
    /* 强制校验 self 首参：多态分派必须把实例指针传回来 */
    if (self->ops != &hubport_ops)
        return -1;
    self->name = "hubport";
    return 0;
}

static ssize_t hubport_read(struct netdev *self, uint8_t *buf, size_t buflen)
{
    struct hubport_netdev *hp = container_of(self, struct hubport_netdev, base);
    (void)hp;
    (void)buflen;
    buf[0] = (uint8_t)'h';
    self->rx_bytes += 1;
    return 1;
}

static int hubport_send(struct netdev *self, const uint8_t *buf, size_t len)
{
    struct hubport_netdev *hp = container_of(self, struct hubport_netdev, base);
    (void)buf;
    return hp->fd >= 0 ? (int)len : -1;
}

static void hubport_periodic(struct netdev *self)
{
    (void)self;
}

static const struct netdev_ops hubport_ops = {   /* C99 指定初始化，static const */
    .init     = hubport_init,
    .read     = hubport_read,
    .send     = hubport_send,
    .periodic = hubport_periodic,
};

/* ===== 子类 2：loopback（验证多态：同一基指针分派到不同实现） ===== */

struct loopback_netdev {
    struct netdev base;
    uint8_t loop_buf[64];
};

static int loopback_init(struct netdev *self)
{
    if (self->ops != &loopback_ops)
        return -1;
    self->name = "loopback";
    return 0;
}

static ssize_t loopback_read(struct netdev *self, uint8_t *buf, size_t buflen)
{
    (void)buf;
    (void)buflen;
    self->rx_bytes += 0;
    return 0;
}

static int loopback_send(struct netdev *self, const uint8_t *buf, size_t len)
{
    struct loopback_netdev *lb = container_of(self, struct loopback_netdev, base);
    memcpy(lb->loop_buf, buf, len);
    return (int)len;
}

static void loopback_periodic(struct netdev *self)
{
    (void)self;
}

static const struct netdev_ops loopback_ops = {
    .init     = loopback_init,
    .read     = loopback_read,
    .send     = loopback_send,
    .periodic = loopback_periodic,
};

/* ===== 用例 ===== */

/* 继承：向上转型后基指针与子类首成员地址相同 */
START_TEST(test_inherit_upcast)
{
    struct hubport_netdev hp;
    struct netdev *nd = (struct netdev *)&hp;

    ck_assert_ptr_eq(nd, &hp.base);
}
END_TEST

/* 多态：两张 static const ops 表，同型基指针分派到各自实现 */
START_TEST(test_polymorphism)
{
    struct hubport_netdev hp;
    struct loopback_netdev lb;
    struct netdev *a = &hp.base;
    struct netdev *b = &lb.base;
    uint8_t pkt[16] = "hello";

    hp.base.ops = &hubport_ops;
    lb.base.ops = &loopback_ops;
    hp.fd = 1;

    ck_assert_int_eq(a->ops->send(a, pkt, 5), 5);   /* 走 hubport_send */
    ck_assert_int_eq(b->ops->send(b, pkt, 5), 5);   /* 走 loopback_send */
    ck_assert_int_eq(memcmp(lb.loop_buf, pkt, 5), 0);
    ck_assert_ptr_eq(a->ops->init, hubport_init);
    ck_assert_ptr_eq(b->ops->init, loopback_init);
}
END_TEST

/* self 首参：回调拿到的 self 必须是调用时的实例本身 */
START_TEST(test_self_first_arg)
{
    struct hubport_netdev hp;
    struct netdev *nd = &hp.base;

    memset(&hp, 0, sizeof(hp));
    hp.base.ops = &hubport_ops;
    hp.fd = 3;
    ck_assert_int_eq(nd->ops->init(nd), 0);          /* init 内校验 self->ops->init 分派正确 */
    ck_assert_str_eq(nd->name, "hubport");
    ck_assert_ptr_eq(nd, &hp.base);
}
END_TEST

/* container_of：回调只拿到基指针，反推子类私有 fd */
START_TEST(test_container_of)
{
    struct hubport_netdev hp;
    struct netdev *nd = &hp.base;
    struct hubport_netdev *back;
    uint8_t buf[8];
    ssize_t n;

    memset(&hp, 0, sizeof(hp));
    hp.base.ops = &hubport_ops;
    hp.fd = 7;
    back = container_of(nd, struct hubport_netdev, base);

    ck_assert_ptr_eq(back, &hp);
    ck_assert_int_eq(back->fd, 7);

    n = nd->ops->read(nd, buf, sizeof(buf));         /* read 内部也走 container_of */
    ck_assert_int_eq(n, 1);
    ck_assert_int_eq(nd->rx_bytes, 1);
    ck_assert_int_eq(nd->ops->send(nd, buf, (size_t)n), 1);   /* send 用子类 fd 判断 */
}
END_TEST

/* 无 malloc：static 实例就地初始化后直接可用 */
START_TEST(test_init_no_malloc)
{
    static struct hubport_netdev g_hp;
    struct netdev *nd = &g_hp.base;

    g_hp.base.ops = &hubport_ops;
    g_hp.fd = -1;                                    /* 无效 fd，send 应失败 */
    ck_assert_int_eq(nd->ops->init(nd), 0);
    ck_assert_int_lt(nd->ops->send(nd, (const uint8_t *)"x", 1), 0);
}
END_TEST

static Suite *make_suite(void)
{
    Suite *s = suite_create("c-style-oo");
    TCase *tc = tcase_create("ops");

    tcase_add_test(tc, test_inherit_upcast);
    tcase_add_test(tc, test_polymorphism);
    tcase_add_test(tc, test_self_first_arg);
    tcase_add_test(tc, test_container_of);
    tcase_add_test(tc, test_init_no_malloc);
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
