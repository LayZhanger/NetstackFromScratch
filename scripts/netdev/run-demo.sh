#!/bin/sh
# 一键启动阶段零 demo：构建 → 启动 hub + 进程 A + 进程 B → 聚合显示日志。
# hub 固定配置 N 个端口，每个端口一个独立 socket 路径 <sock_base>-<i>.sock；
# 进程 A 连 hub-0.sock、进程 B 连 hub-1.sock（netdev CLIENT 角色）。
# 用法：scripts/netdev/run-demo.sh [hub 丢包率参数...]
#   例：scripts/netdev/run-demo.sh 0 40   （端口1 丢 40% 帧）
# Ctrl+C 停止全部进程（终端向前台进程组发信号，trap 兜底清理）。

set -e

BASE=/tmp/netstack-hub
LOG=/tmp/netstack-demo
NPORTS=2
SCRIPT_DIR=$(dirname "$0")
cd "$SCRIPT_DIR/../.."     # 定位仓库根：从任意目录运行均可

if [ ! -f dist/bin/hub ] || [ ! -f dist/bin/netdev_demo ]; then
    echo "== 首次运行，先构建 =="
    ./build.sh >/dev/null
fi

rm -f "$BASE"-*.sock "$LOG"*.log

echo "== 启动 hub =="
dist/bin/hub "$BASE" "$NPORTS" "$@" > "$LOG-hub.log" 2>&1 &
HUB_PID=$!
sleep 0.3

echo "== 启动进程 A =="
dist/bin/netdev_demo "$BASE-0.sock" A > "$LOG-A.log" 2>&1 &
A_PID=$!
sleep 0.3

echo "== 启动进程 B =="
dist/bin/netdev_demo "$BASE-1.sock" B > "$LOG-B.log" 2>&1 &
B_PID=$!

CLEANED=0
cleanup() {
    [ "$CLEANED" -eq 1 ] && return
    CLEANED=1
    echo
    echo "== 已停止（日志保留在 $LOG*.log）=="
    kill "$HUB_PID" "$A_PID" "$B_PID" "$TAIL_PID" 2>/dev/null || true
    rm -f "$BASE"-*.sock
}
trap cleanup INT TERM EXIT

echo "== 全部就绪，聚合日志如下（Ctrl+C 停止）=="
tail -f "$LOG-A.log" "$LOG-B.log" "$LOG-hub.log" &
TAIL_PID=$!
# sleep 会被信号中断 → 执行 trap 清理；tail 死后循环退出
while kill -0 "$TAIL_PID" 2>/dev/null; do
    sleep 0.5
done
