#ifndef ICMP_FACTORY_H
#define ICMP_FACTORY_H

/*
 * icmp 层工厂占位（00-计划.md 功能清单，RFC 编号见文件头引用）。
 * 抽象接口与具体后端待 icmp 阶段按 RFC 实现；本文件先固化「工厂 + 抽象指针」的模块形态：
 * 模块根 <layer>_factory.{h,c}，impl/ 放具体后端。
 */

struct icmp_device;   /* icmp 层设备抽象，待实现时定义完整结构；当前仅占位前向声明 */

/*
 * 按类型名创建 icmp 层实例（工厂 malloc，成功返回非 NULL）。
 * 待 icmp 阶段实现；当前返回 NULL。
 * params：具体后端各自参数结构，const void * 统一入口。
 */
struct icmp_device *icmp_create(const char *type, const void *params);

#endif /* ICMP_FACTORY_H */
