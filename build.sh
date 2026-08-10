#!/bin/sh
# 总编译脚本：配置 → 编译 → 运行测试
# 产物全部输出到 dist/（已 gitignore），不污染源码树
set -e

BUILD_DIR=dist/tests

cmake -S tests -B "$BUILD_DIR"
cmake --build "$BUILD_DIR"

echo "== 运行测试 =="
"$BUILD_DIR/unit/test_ops"
