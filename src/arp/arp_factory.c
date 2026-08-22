/*
 * arp_factory.c：arp 层工厂占位实现（c-style skill §7 模块工厂约定）。
 * 待 arp 阶段：impl/ 新增后端，再在注册表注册。
 */

#include "arp_factory.h"
#include <stddef.h>

struct arp_device *arp_create(const char *type, const void *params)
{
	(void)type;
	(void)params;
	return NULL;   /* TODO: arp 阶段实现 */
}
