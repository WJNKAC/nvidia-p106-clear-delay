#!/bin/bash
# clean-wine.sh —— 彻底清理所有 wine 进程（含 Windows 侧 explorer/services 等）
#
# 为什么需要专门脚本：
#   Wine 的 Windows 进程命令行是这样的，**完全不含 "wine" 字样**：
#       C:\windows\system32\explorer.exe /desktop
#       C:\windows\system32\services.exe
#       C:\windows\system32\plugplay.exe
#       C:\windows\system32\svchost.exe -k LocalServiceNetworkRestricted
#       C:\windows\system32\rpcss.exe
#       C:\windows\system32\lsass.exe
#   用 grep "wine" 找会全部漏掉，但它们的 /proc/PID/exe 指向 wine-preloader。
#
#   正确做法：① 先 wineserver -k 让它自己按序收尾
#             ② wineserver -w 等它退完
#             ③ 再扫 /proc/*/exe 兜底（真卡死的才 kill -9）
#
# 用法：
#   WINEPREFIX=/path/to/prefix ./clean-wine.sh [wine安装根目录]
#   或直接 ./clean-wine.sh   （会用默认路径）

set -u

WINEBASE="${2:-/media/union/HDD_450.76GiB/data/software/apps/wine}"
DEF_PREFIX="$WINEBASE/main/x86_64/prefixs/wine-11.19-staging-amd64-wow64-0"
PREFIX="${WINEPREFIX:-$DEF_PREFIX}"

# 找 wineserver
WINESERVER=""
if [ -x "$PREFIX/../../../decomps/wine-11.19-staging-amd64-wow64/bin/wineserver" ]; then
    WINESERVER="$PREFIX/../../../decomps/wine-11.19-staging-amd64-wow64/bin/wineserver"
else
    WS=$(find "$WINEBASE" -name wineserver -type f 2>/dev/null | head -1)
    [ -n "$WS" ] && WINESERVER="$WS"
fi

list_wine_pids() {
    for p in /proc/[0-9]*; do
        pid=${p##*/}
        e=$(readlink "$p/exe" 2>/dev/null) || continue
        case "$e" in *"$WINEBASE"*) echo "$pid" ;; esac
    done
}

echo "=== wine 根目录: $WINEBASE"
echo "=== WINEPREFIX : $PREFIX"
echo "=== wineserver : ${WINESERVER:-未找到}"
echo

echo "=== 1) 清理前 ==="
BEFORE=$(list_wine_pids)
if [ -z "$BEFORE" ]; then
    echo "  无 wine 进程"
else
    for pid in $BEFORE; do
        cmd=$(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null | cut -c1-64)
        printf "  pid=%-7s %s\n" "$pid" "$cmd"
    done
    echo "  共 $(echo "$BEFORE" | wc -l) 个"
fi
echo

echo "=== 2) wineserver -k（让 wine 自己按序退出）==="
if [ -n "$WINESERVER" ]; then
    "$WINESERVER" -k 2>/dev/null
    rc=$?
    if [ $rc -eq 1 ]; then
        echo "  返回 1 = 本来就没有 wineserver 在跑（正常）"
    else
        echo "  返回 $rc"
    fi
    echo "=== 3) wineserver -w（等它真正退完，最多 15s）==="
    timeout 15 "$WINESERVER" -w 2>/dev/null
    echo "  返回 $?"
else
    echo "  ★ 找不到 wineserver，跳过，将直接按 PID 清理"
fi
echo

echo "=== 4) 按 exe 扫描兜底（多轮）==="
ROUND=0
while :; do
    LEFT=$(list_wine_pids)
    [ -z "$LEFT" ] && break
    ROUND=$((ROUND+1))
    if [ $ROUND -gt 5 ]; then
        echo "  ★ 已 5 轮仍有残留，停止"
        break
    fi
    echo "  第 $ROUND 轮：$(echo "$LEFT" | wc -l) 个"
    for pid in $LEFT; do
        cmd=$(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null | cut -c1-56)
        echo "    kill -9 $pid  $cmd"
        kill -9 "$pid" 2>/dev/null
    done
    sleep 2
done
echo

echo "=== 5) 清理残留 socket/lock 目录 ==="
UIDIR="/tmp/.wine-$(id -u)"
if [ -d "$UIDIR" ]; then
    # 只删没有进程连接的 server-* 目录
    for d in "$UIDIR"/server-*; do
        [ -d "$d" ] || continue
        n=$(ls -l /proc/*/fd/* 2>/dev/null | grep -c "$d" || true)
        if [ "${n:-0}" -eq 0 ]; then
            rm -rf "$d" && echo "  已删孤儿目录: $d"
        else
            echo "  保留（有 $n 个连接）: $d"
        fi
    done
    rmdir "$UIDIR" 2>/dev/null && echo "  已删空目录: $UIDIR"
fi
echo

echo "=== 6) 最终确认 ==="
FINAL=$(list_wine_pids)
if [ -z "$FINAL" ]; then
    echo "  ✓ 无 wine 进程"
else
    echo "  ★ 仍有: $FINAL"
    for pid in $FINAL; do
        echo "    pid=$pid state=$(awk '{print $3}' /proc/$pid/stat 2>/dev/null) wchan=$(cat /proc/$pid/wchan 2>/dev/null)"
    done
fi
echo
echo "=== GPU ==="
nvidia-smi --query-gpu=utilization.gpu,clocks.mem,clocks.sm,power.draw,memory.used --format=csv,noheader
