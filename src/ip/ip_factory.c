/*
 * ip_factory.c：ip 层工厂占位实现（c-style skill §7 模块工厂约定）。
 * 待 ip 阶段：impl/ 新增后端，再在注册表注册。
 */

#include "ip_factory.h"
#include <stddef.h>

struct ip_device *ip_create(const char *type, const void *params)
{
	(void)type;
	(void)params;
	return NULL;   /* TODO: ip 阶段实现 */
}
