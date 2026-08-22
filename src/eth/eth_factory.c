/*
 * eth_factory.c：eth 层工厂占位实现（c-style skill §7 模块工厂约定）。
 * 待 eth 阶段：impl/ 新增后端，再在注册表注册。
 */

#include "eth_factory.h"
#include <stddef.h>

struct eth_device *eth_create(const char *type, const void *params)
{
	(void)type;
	(void)params;
	return NULL;   /* TODO: eth 阶段实现 */
}
