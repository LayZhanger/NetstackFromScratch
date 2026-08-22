#ifndef CONTAINER_OF_H
#define CONTAINER_OF_H

/*
 * container_of：内核标志性宏。
 * 子结构内嵌基结构（继承），拿到基结构指针后反推包含它的完整结构。
 * 用法：container_of(nd, struct unix_socket_netdev, base)
 *   nd    = 指向内嵌成员 base 的指针
 *   type  = 外层结构类型
 *   member = 内嵌成员名（须与 type 中的成员一致）
 * 内核参考：include/linux/container_of.h
 */

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - (char *)&((type *)0)->member))

#endif /* CONTAINER_OF_H */
