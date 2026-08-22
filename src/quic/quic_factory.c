/*
 * quic_factory.c：quic 层工厂占位实现（c-style skill §7 模块工厂约定）。
 * 待 quic 阶段：impl/ 新增后端，再在注册表注册。
 */

#include "quic_factory.h"
#include <stddef.h>

struct quic_device *quic_create(const char *type, const void *params)
{
	(void)type;
	(void)params;
	return NULL;   /* TODO: quic 阶段实现 */
}
