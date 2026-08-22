#ifndef NETDEV_FACTORY_H
#define NETDEV_FACTORY_H

#include "netdev.h"

/*
 * netdev 工厂：按设备类型名统一创建/销毁（01-架构.md §3 抽象原则）。
 * 实现可能不止一种（unix_socket、未来 tap 等），上层只依赖 netdev.h + netdev_factory.h，
 * 不再 include 具体后端头。加新后端 = impl/ 新增后端 + 注册表加一行（netdev_factory.c）。
 */

/* 设备类型注册项：每个后端在注册表里占一项 */
struct netdev_type {
	const char *type;                               /* "unix_socket"、"tap"、... */
	const struct netdev_ops *ops;                   /* 本后端虚表，netdev_destroy 反查类型用 */
	struct netdev *(*create)(const void *params);   /* 后端工厂：malloc 实例并 init */
	void (*destroy)(struct netdev *self);            /* 后端销毁，与 create 配对 */
};

/*
 * 按 type 名创建 netdev（工厂 malloc 实例，已 init，可直接用）。
 * 返回基类指针；type 未命中或后端创建失败返回 NULL。
 * 创建成功者所有权归调用方，用 netdev_destroy 释放。
 */
struct netdev *netdev_create(const char *type, const void *params);

/*
 * 销毁 netdev：按实例虚表反查注册项 → 调后端 destroy（关闭 fd、释放队列）+ free 实例。
 * self 必须来自 netdev_create，未创建成功者不要调用。
 */
void netdev_destroy(struct netdev *self);

#endif /* NETDEV_FACTORY_H */
