#!/bin/sh
# 总编译脚本：配置 → 编译 → 运行测试
# 产物全部输出到 dist/（已 gitignore），不污染源码树
set -e

BUILD_DIR=dist/tests

cmake -S tests -B "$BUILD_DIR"
cmake --build "$BUILD_DIR"

echo "== 编译工具与演示进程 =="
mkdir -p dist/bin
gcc -std=c99 -Wall -Wextra -O2 -o dist/bin/hub hub/hub.c
gcc -std=c99 -Wall -Wextra -O2 -Isrc/include -Isrc/netdev \
    -o dist/bin/netdev_demo src/app/netdev/demo.c src/netdev/netdev.c src/netdev/hubport.c

echo "== 运行测试 =="
"$BUILD_DIR/unit/test_ops"
"$BUILD_DIR/unit/test_hubport"
