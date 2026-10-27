#!/bin/bash
# patchtest.sh <newvalue_hex> [frames]
# 整个 run 都在干预下：先起改写着，再起 n83b，等它自然跑完读统计。
# 用 -G（glFinish）测纯 GPU 时间。
set -u
NEW="${1:?用法: patchtest.sh <newvalue_hex> [frames]}"
FRAMES="${2:-200}"
XA=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
export DISPLAY=:0 XAUTHORITY="$XA" __GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1
cd <workdir>/analysis/n83a || exit 3

echo "### 实参 0xF0 -> $NEW （帧数 $FRAMES，-G 纯 GPU 时间）"

# 占位进程：先起一个短跑，拿到 pid 供 pbpatch2 attach（pbpatch2 需要目标存在）
# 实际做法：先起长跑，再起改写着，等长跑结束
./n83b_new -M 7 -c 400 -n 400 -f "$FRAMES" -W 3 -q -G >/tmp/pt_run.log 2>&1 &
P=$!
sleep 0.8
../onheap/pbpatch4 $P 0x3998 "$NEW" 60 >/tmp/pt_patch.out 2>/tmp/pt_patch.err &
PP=$!
wait $P 2>/dev/null
kill $PP 2>/dev/null; wait $PP 2>/dev/null

echo "  改写: $(head -1 /tmp/pt_patch.err 2>/dev/null)"
grep -oE "rp_p50=[0-9.]+|total_p50=[0-9.]+" /tmp/pt_run.log | tr '\n' ' '; echo
