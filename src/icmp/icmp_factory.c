/*
 * icmp_factory.c：icmp 层工厂占位实现（c-style skill §7 模块工厂约定）。
 * 待 icmp 阶段：impl/ 新增后端，再在注册表注册。
 */

#include "icmp_factory.h"
#include <stddef.h>

struct icmp_device *icmp_create(const char *type, const void *params)
{
	(void)type;
	(void)params;
	return NULL;   /* TODO: icmp 阶段实现 */
}
