#!/bin/bash
# heaven_ab.sh <base|patched> [countsecs]
# 用代码补丁跑 Heaven，用"纯计数器"测 0x3998 的速率作为帧率代理。
#
# 为什么这次 A/B 可信（与之前数据补丁那轮不同）：
#   代码补丁是在驱动里把立即数改掉，运行时零开销；
#   计数器进程在两次运行中的开销完全一致 ⇒ 命中率可直接横向比较。
set -u
MODE="${1:?用法: heaven_ab.sh <base|patched> [countsecs]}"
CSECS="${2:-15}"

H="<apps-dir>/unigine_heaven/main/x86_64/decomps/Unigine_Heaven-4.0"
XA=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
export DISPLAY=:0 XAUTHORITY="$XA" vblank_mode=0
unset __GLX_VENDOR_LIBRARY_NAME __NV_PRIME_RENDER_OFFLOAD DRI_PRIME
cd "$H/bin" || exit 2

pkill -9 -x heaven_x64 2>/dev/null
sleep 1

PATCHENV=()
if [ "$MODE" = "patched" ]; then
    PATCHENV=(env "PATCH=0x15c376d=10;0x1842137=10" \
                  "LD_PRELOAD=<workdir>/analysis/shim/patchpb.so")
fi

"${PATCHENV[@]}" primusrun ./heaven_x64 \
    -project_name Heaven -data_path ../ -engine_config ../data/heaven_4.0.cfg \
    -system_script heaven/unigine.cpp -sound_app openal -video_app opengl \
    -video_multisample 0 -video_fullscreen 0 -video_mode 6 \
    -extern_define ,RELEASE,LANGUAGE_CH,QUALITY_HIGH,TESSELLATION_DISABLED \
    -extern_plugin ,GPUMonitor >"/tmp/ab_$MODE.log" 2>&1 &

sleep 38
P=$(pgrep -x heaven_x64 | head -1)
if [ -z "$P" ]; then echo "!! Heaven 没起来"; tail -8 "/tmp/ab_$MODE.log"; exit 3; fi

echo "=== 模式=$MODE  pid=$P ==="
grep -E "patchpb" "/tmp/ab_$MODE.log" | head -4

# 三采样功耗
PWR=$(for i in 1 2 3; do nvidia-smi --query-gpu=power.draw --format=csv,noheader; sleep 1; done | tr '\n' ' ')

# 纯计数（零写入，开销与另一轮一致）
HITS=$(<workdir>/analysis/onheap/pbpatch4 $P 0x3998 0xF0 "$CSECS" count 2>&1 | grep -oE "命中=[0-9]+" | head -1)
echo "功耗(3次): $PWR"
echo "$HITS  → 速率 $(echo "$HITS" | grep -oE "[0-9]+") / $CSECS s"

pkill -9 -x heaven_x64 2>/dev/null
