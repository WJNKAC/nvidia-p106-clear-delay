#!/bin/bash
# pbwin.sh <批次次数c> [秒数]
# 启动 n83b(-M 7 -c <c>)，抓一次 ctx，然后用零干扰方式测 pushbuffer 的
#   写入速率 / 未消费窗口(put-rp)
# 输出可直接跨版本比较。
set -u
C="${1:?用法: pbwin.sh <c> [secs]}"
SECS="${2:-3}"
VER=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | tr -d ' ')
case "$VER" in
  515*) WOFF=0xfbcc30; PUT=0x358; RP=0x210 ;;
  525*) WOFF=0x15761b0; PUT=0x1130; RP=0x1240 ;;
  *) echo "未知驱动 $VER"; exit 2 ;;
esac
XA=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
export DISPLAY=:0 XAUTHORITY="$XA" __GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1
cd <workdir>/analysis/n83a || exit 3
PY=/usr/bin/python3

./n83b_new -M 7 -c "$C" -n 400 -f 20000 -W 5 -q >/tmp/pbw_$C.log 2>&1 &
P=$!
sleep 1.5
# 抓 ctx（唯一的 rdi）
CTX=$(../onheap/entryst $P nvidia-glcore $WOFF 1 $P rdi 2>/dev/null | grep -E "^  0x" | head -1 | awk '{print $1}')
if [ -z "$CTX" ]; then echo "c=$C 抓 ctx 失败"; kill $P 2>/dev/null; exit 4; fi
# 零干扰测量（此时 ptrace 已 detach）
$PY ../tools/pbrate.py $P "$CTX" $PUT $RP "$SECS" 20 > /tmp/pbw_$C.rate 2>&1
kill $P 2>/dev/null; wait $P 2>/dev/null
echo "===== 驱动 $VER  c=$C  ====="
grep -oE "total_p50=[0-9.]+|submit_p50=[0-9.]+|map_p50=[0-9.]+" /tmp/pbw_$C.log | tr '\n' ' '; echo
cat /tmp/pbw_$C.rate
