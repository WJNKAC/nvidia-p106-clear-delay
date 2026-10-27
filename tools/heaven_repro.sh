#!/bin/bash
# heaven_repro.sh <mode> [newval_hex] [seconds]
#   mode = base    : 不干预，跑 Heaven benchmark，读 FPS
#   mode = patch   : 全程把 0x3998 实参改成 newval，读 FPS
#   mode = readonly: 全程只读不改（newval=原值 0xF0），作为零干扰对照
#
# 用 primusrun 呈现，绕开 Xwayland present 的 60fps 节流（报告 165）。
set -u
MODE="${1:?用法: heaven_repro.sh <base|patch|readonly> [newval] [secs]}"
NEWVAL="${2:-0x10}"
SECS="${3:-120}"

H="<apps-dir>/unigine_heaven/main/x86_64/decomps/Unigine_Heaven-4.0"
XA=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
export DISPLAY=:0
export XAUTHORITY="$XA"
# 用 primusrun 就必须清掉这些，否则抢 GLX 黑屏（报告 159）
unset __GLX_VENDOR_LIBRARY_NAME __NV_PRIME_RENDER_OFFLOAD DRI_PRIME
export vblank_mode=0

cd "$H/bin" || { echo "找不到 heaveh bin"; exit 2; }

LOG="/tmp/hv_$MODE.log"
rm -f "$LOG"

echo "=== 模式=$MODE  实参=$NEWVAL  DISPLAY=$DISPLAY  XAUTH=$XA ==="
echo "=== 驱动: $(nvidia-smi --query-gpu=driver_version --format=csv,noheader) ==="

primusrun ./heaven_x64 \
    -project_name Heaven -data_path ../ -engine_config ../data/heaven_4.0.cfg \
    -system_script heaven/unigine.cpp -sound_app openal -video_app opengl \
    -video_multisample 0 -video_fullscreen 0 -video_mode 6 \
    -extern_define ,RELEASE,LANGUAGE_CH,QUALITY_HIGH,TESSELLATION_DISABLED \
    -extern_plugin ,GPUMonitor -benchmark \
    > "$LOG" 2>&1 &
PID=$!
sleep 6

if ! kill -0 $PID 2>/dev/null; then
    echo "!! Heaven 已退出，日志尾部："; tail -15 "$LOG"; exit 3
fi
echo "=== Heaven pid=$PID ==="

case "$MODE" in
  base)
      ;;
  patch)
      ../onheap/pbpatch4 $PID 0x3998 "$NEWVAL" "$SECS" >/tmp/hv_patch.out 2>/tmp/hv_patch.err &
      PP=$!
      echo "=== 改写启动: $(cat /tmp/hv_patch.err)"
      ;;
  readonly)
      ../onheap/pbpatch4 $PID 0x3998 0xF0 "$SECS" >/tmp/hv_patch.out 2>/tmp/hv_patch.err &
      PP=$!
      echo "=== 只读对照: $(cat /tmp/hv_patch.err)"
      ;;
esac

wait $PID 2>/dev/null
kill ${PP:-0} 2>/dev/null; wait ${PP:-0} 2>/dev/null

echo "=== 结果 ==="
grep -iE "^FPS:|^Score|^Min FPS|^Max FPS|Benchmark" "$LOG" | head -12
[ -f /tmp/hv_patch.out ] && echo "改写统计: $(tail -1 /tmp/hv_patch.out)"
