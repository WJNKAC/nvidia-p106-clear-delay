#!/bin/bash
# 在同一负载下采集"wait / ensure_space / 每批成本"，输出可直接对比的文件
# 用法: measure_batch.sh <驱动版本号> <输出前缀>
set -u
VER="${1:?用法: measure_batch.sh <ver> <outprefix>}"
OUT="${2:-<workdir>/analysis/batch_$VER}"
N83=<workdir>/analysis/n83a/n83b_new
ENT=<workdir>/analysis/onheap/entryst
XA=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
export DISPLAY=:0 XAUTHORITY="$XA" __GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1

case "$VER" in
  515*) WAIT=0xfbcc30; ENS=0xfbed80; TIMER=6f50 ;;
  525*) WAIT=0x15761b0; ENS=0x1578280; TIMER=6f88 ;;
  *) echo "未知版本 $VER"; exit 2 ;;
esac

PID=0
start_bg() {          # $1=帧数；设置全局 PID
    $N83 -M 7 -c 400 -n 400 -f "$1" -W 5 -q >"$OUT.n83b.log" 2>&1 &
    PID=$!
}
stop_bg() { [ "$PID" -gt 0 ] && kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=0; }

echo "===== $VER  wait=$WAIT  ensure_space=$ENS  timer=+0x$TIMER ====="
cd <workdir>/analysis/n83a || exit 3

# ---- 1) 基准（无 ptrace）----
echo "-- 基准 x3：total_p50 / submit_p50 / map_p50 / fps --" | tee "$OUT.base.txt"
for i in 1 2 3; do
    start_bg 2000; wait "$PID"; PID=0
    grep -oE "total_p50=[0-9.]+|submit_p50=[0-9.]+|map_p50=[0-9.]+|fps=[0-9.]+" "$OUT.n83b.log" \
        | tr '\n' ' ' | tee -a "$OUT.base.txt"; echo | tee -a "$OUT.base.txt"
done

# ---- 2) perf 归因 ----
echo <user> | sudo -S env DISPLAY=:0 XAUTHORITY="$XA" \
  __GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 \
  perf record -F 997 -g --call-graph=dwarf -o "$OUT.perf.data" -- \
  $N83 -M 7 -c 400 -n 400 -f 2500 -W 5 -q >"$OUT.perfrun.log" 2>"$OUT.perf.err"
echo "-- perf 跑：$(grep -oE 'total_p50=[0-9.]+|fps=[0-9.]+' "$OUT.perfrun.log" | tr '\n' ' ') --"
echo <user> | sudo -S perf report -i "$OUT.perf.data" --stdio --no-call-graph --sort dso --percent-limit 0.5 2>/dev/null \
  | grep -vE "^#" > "$OUT.dso.txt"
echo <user> | sudo -S perf script -i "$OUT.perf.data" --show-mmap-events --no-inline 2>/dev/null \
  | grep -i "nvidia-glcore" | head -1 > "$OUT.mmap.txt"
echo "-- DSO 分解 --"; head -6 "$OUT.dso.txt"
echo "-- glcore 基址 --"; cat "$OUT.mmap.txt"

# ---- 3) wait 计数 + 调用点 ----
start_bg 6000; sleep 1.5
echo "-- wait 入口命中 --" | tee "$OUT.waitcnt.txt"
$ENT $PID nvidia-glcore $WAIT 3 $PID 2>&1 | grep "命中总数" | tee -a "$OUT.waitcnt.txt"
echo "-- wait 调用点 --" | tee "$OUT.waitsite.txt"
$ENT $PID nvidia-glcore $WAIT 2 $PID ret 2>&1 | grep -E "^  0x" | head -5 | tee -a "$OUT.waitsite.txt"
stop_bg

# ---- 4) ensure_space 计数 + rsi ----
start_bg 6000; sleep 1.5
echo "-- ensure_space 入口命中 --" | tee "$OUT.enscnt.txt"
$ENT $PID nvidia-glcore $ENS 3 $PID 2>&1 | grep -E "命中总数|不同取值" | tee -a "$OUT.enscnt.txt"
echo "-- ensure_space rsi 取值 --" | tee "$OUT.ensrsi.txt"
$ENT $PID nvidia-glcore $ENS 2 $PID rsi 2>&1 | grep -E "^  0x" | head -6 | tee -a "$OUT.ensrsi.txt"
stop_bg

# ---- 5) wait 自计时器（仅时间戳路径）----
start_bg 6000; sleep 1.5
echo "-- wait 自计时器 --" | tee "$OUT.timer.txt"
$ENT $PID nvidia-glcore $WAIT 3 $PID "timer:$TIMER" 2>&1 | grep -E "平均|累计" | tee -a "$OUT.timer.txt"
stop_bg

echo "===== 完成：$OUT.* ====="
