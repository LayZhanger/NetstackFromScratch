/*
 * udp_factory.c：udp 层工厂占位实现（c-style skill §7 模块工厂约定）。
 * 待 udp 阶段：impl/ 新增后端，再在注册表注册。
 */

#include "udp_factory.h"
#include <stddef.h>

struct udp_device *udp_create(const char *type, const void *params)
{
	(void)type;
	(void)params;
	return NULL;   /* TODO: udp 阶段实现 */
}
