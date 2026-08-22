/*
 * netdev.c：设备抽象基类实现（01-架构.md §3）。
 * 基类只做分派与回调注册，不持有任何设备状态；实例/后端由子类提供（c-style skill §3.1）。
 */

#include "netdev.h"

int netdev_init(struct netdev *self)
{
	return self->ops->init(self);
}

int netdev_start(struct netdev *self)
{
	return self->ops->start(self);
}

int netdev_send(struct netdev *self, const uint8_t *buf, size_t len)
{
	return self->ops->send(self, buf, len);
}

void netdev_stop(struct netdev *self)
{
	self->ops->stop(self);
}

void netdev_set_rx_handler(struct netdev *self, netdev_rx_handler_t h)
{
	self->rx_handler = h;
}
