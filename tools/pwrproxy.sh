#!/bin/bash
# pwrproxy.sh —— 验证"功耗能否作为帧率代理"，并给出 Heaven 的 A/B
#
# 逻辑：如果 功耗 ∝ 帧率，那它就能替代 FPS 读数（我们在 Heaven 上拿不到 FPS）。
# 先在 n83b 上验证这个代理（那里有精确的 rp_p50 可对照），再用于 Heaven。
set -u
XA=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
export DISPLAY=:0 XAUTHORITY="$XA" __GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1

sample_pwr() {   # $1=秒数，输出平均值
    local n=0 sum=0 v
    for _ in $(seq 1 "$1"); do
        v=$(nvidia-smi --query-gpu=power.draw --format=csv,noheader,nounits | tr -d ' ')
        sum=$(awk -v a="$sum" -v b="$v" 'BEGIN{print a+b}')
        n=$((n+1))
        sleep 1
    done
    awk -v s="$sum" -v n="$n" 'BEGIN{printf "%.1f", s/n}'
}

echo "===== 第一步：在 n83b 上验证功耗代理 ====="
cd <workdir>/analysis/n83a || exit 2
for label in "240:" "16:0x15c376d=10;0x1842137=10"; do
    name="${label%%:*}"
    pt="${label#*:}"
    if [ -n "$pt" ]; then
        env PATCH="$pt" LD_PRELOAD=<workdir>/analysis/shim/patchpb.so \
            ./n83b_new -M 7 -c 400 -n 400 -f 4000 -W 3 -q -G >/tmp/pw_$name.log 2>&1 &
    else
        ./n83b_new -M 7 -c 400 -n 400 -f 4000 -W 3 -q -G >/tmp/pw_$name.log 2>&1 &
    fi
    m=$!
    sleep 2
    p=$(sample_pwr 3)
    wait $m 2>/dev/null
    rp=$(grep -oE "rp_p50=[0-9.]+" /tmp/pw_$name.log | head -1)
    sp=$(grep -oE "submit_p50=[0-9.]+" /tmp/pw_$name.log | head -1)
    printf "  %-6s 功耗=%sW  %s %s\n" "$name" "$p" "$rp" "$sp"
done

echo
echo "===== 第二步：Heaven 上做同样的 A/B（功耗 + 命令速率）====="
H="<apps-dir>/unigine_heaven/main/x86_64/decomps/Unigine_Heaven-4.0"
unset __GLX_VENDOR_LIBRARY_NAME __NV_PRIME_RENDER_OFFLOAD
export vblank_mode=0
cd "$H/bin" || exit 3

for mode in base patched; do
    pkill -9 -x heaven_x64 2>/dev/null
    sleep 1
    if [ "$mode" = patched ]; then
        env PATCH="0x15c376d=10;0x1842137=10" LD_PRELOAD=<workdir>/analysis/shim/patchpb.so \
            primusrun ./heaven_x64 -project_name Heaven -data_path ../ \
            -engine_config ../data/heaven_4.0.cfg -system_script heaven/unigine.cpp \
            -sound_app openal -video_app opengl -video_multisample 0 -video_fullscreen 0 \
            -video_mode 6 -extern_define ,RELEASE,LANGUAGE_CH,QUALITY_HIGH,TESSELLATION_DISABLED \
            -extern_plugin ,GPUMonitor >"/tmp/hvab_$mode.log" 2>&1 &
    else
        primusrun ./heaven_x64 -project_name Heaven -data_path ../ \
            -engine_config ../data/heaven_4.0.cfg -system_script heaven/unigine.cpp \
            -sound_app openal -video_app opengl -video_multisample 0 -video_fullscreen 0 \
            -video_mode 6 -extern_define ,RELEASE,LANGUAGE_CH,QUALITY_HIGH,TESSELLATION_DISABLED \
            -extern_plugin ,GPUMonitor >"/tmp/hvab_$mode.log" 2>&1 &
    fi
    sleep 40
    P=$(pgrep -x heaven_x64 | head -1)
    if [ -z "$P" ]; then echo "  $mode: Heaven 没起来"; continue; fi
    p=$(sample_pwr 3)
    hits=$(<workdir>/analysis/onheap/pbpatch4 "$P" 0x3998 0xF0 12 count 2>&1 | grep -oE "命中=[0-9]+" | head -1)
    pf=$(grep -cE "0x15c376d  原文 f0  ->  10" "/tmp/hvab_$mode.log" 2>/dev/null || echo 0)
    printf "  %-8s 功耗=%sW  补丁生效=%s  %s/12s\n" "$mode" "$p" "$pf" "$hits"
    pkill -9 -x heaven_x64 2>/dev/null
done
