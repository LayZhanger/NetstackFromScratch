#ifndef UDP_FACTORY_H
#define UDP_FACTORY_H

/*
 * udp 层工厂占位（00-计划.md 功能清单，RFC 编号见文件头引用）。
 * 抽象接口与具体后端待 udp 阶段按 RFC 实现；本文件先固化「工厂 + 抽象指针」的模块形态：
 * 模块根 <layer>_factory.{h,c}，impl/ 放具体后端。
 */

struct udp_device;   /* udp 层设备抽象，待实现时定义完整结构；当前仅占位前向声明 */

/*
 * 按类型名创建 udp 层实例（工厂 malloc，成功返回非 NULL）。
 * 待 udp 阶段实现；当前返回 NULL。
 * params：具体后端各自参数结构，const void * 统一入口。
 */
struct udp_device *udp_create(const char *type, const void *params);

#endif /* UDP_FACTORY_H */
