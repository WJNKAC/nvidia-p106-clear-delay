#!/bin/bash
# upinit.sh —— 用 bpftrace uprobe 统计"每帧调用了多少次某个函数"
#
# 为什么不用 ptrace：
#   ptrace 硬件断点每次命中都要停住目标，热函数会让目标慢几十倍，
#   于是"次/秒"完全不可横向比较。bpftrace uprobe 只在函数入口插一条 nop
#   跳转，开销极小（通常是原速的几 %），可以放心数绝对值。
#
# 统计量：
#   WAIT  = 目标函数入口命中次数（跨全部线程，按 pid 过滤）
#   FRAME = glXSwapBuffers 入口命中次数
#   输出每帧 WAIT 次数 = WAIT / FRAME  ← 这个比值可以跨版本比较
#
# 用法: upinit.sh <pid> <wait_offset_hex> <swap_offset_hex> <secs>
set -u
PID="${1:?用法: upinit.sh <pid> <wait_off> <swap_off> <secs>}"
WOFF="${2:?}"
SOFF="${3:?}"
SECS="${4:-6}"

GLCORE=$(grep -o '/[^ ]*libnvidia-glcore\.so\.[0-9.]*' /proc/$PID/maps | head -1)
GLX=$(grep -o '/[^ ]*libGLX_nvidia\.so\.[0-9.]*' /proc/$PID/maps | head -1)
[ -z "$GLCORE" ] && { echo "找不到 glcore"; exit 2; }
[ -z "$GLX" ] && { echo "找不到 GLX_nvidia"; exit 3; }

echo "# glcore=$GLCORE"
echo "# glx=$GLX"
echo "# wait_off=$WOFF  swap_off=$SOFF  pid=$PID  secs=$SECS"

cat > /tmp/upcnt.bt <<EOF
uprobe:$GLCORE:$WOFF
/pid == $PID/
{ @wait = count(); }

uprobe:$GLX:$SOFF
/pid == $PID/
{ @frame = count(); }

interval:s:$SECS
{ exit(); }
EOF

echo <user> | sudo -S bpftrace /tmp/upcnt.bt 2>/dev/null | awk '
/^@wait:/ {w=$2} /^@frame:/ {f=$2}
END {
  if (w == "") w = 0
  if (f == "") f = 0
  printf "WAIT=%d  FRAME=%d", w, f
  if (f > 0) printf "  每帧 WAIT = %.1f 次", w / f
  printf "\n"
}'
