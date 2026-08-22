/*
 * netdev_factory.c：netdev 工厂实现（01-架构.md §3 抽象原则）。
 * 注册表罗列已知后端：netdev_create 线性查找 type 名分派；netdev_destroy 按实例虚表
 * 反查注册项调后端 destroy（c-style skill §7 模块工厂约定）。
 * 加新后端：impl/ 新增后端（实现 create/destroy、暴露 ops），再在 netdev_types[] 加一行。
 */

#include <string.h>

#include "netdev_factory.h"
#include "impl/unix_socket_netdev.h"

/* 注册表：已知后端类型（const void *params 在后端内转成各自 params 结构） */
static const struct netdev_type netdev_types[] = {
	{
		.type    = "unix_socket",
		.ops     = &unix_socket_netdev_ops,
		.create  = unix_socket_netdev_create,
		.destroy = unix_socket_netdev_destroy,
	},
};

#define NETDEV_TYPES_LEN \
	(sizeof(netdev_types) / sizeof(netdev_types[0]))

struct netdev *netdev_create(const char *type, const void *params)
{
	size_t i;

	if (!type)
		return NULL;
	for (i = 0; i < NETDEV_TYPES_LEN; i++) {
		if (strcmp(type, netdev_types[i].type) == 0)
			return netdev_types[i].create(params);
	}
	return NULL;
}

void netdev_destroy(struct netdev *self)
{
	size_t i;

	if (!self || !self->ops)
		return;
	for (i = 0; i < NETDEV_TYPES_LEN; i++) {
		if (self->ops == netdev_types[i].ops) {
			netdev_types[i].destroy(self);
			return;
		}
	}
}
