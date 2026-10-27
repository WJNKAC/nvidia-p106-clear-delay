#!/bin/bash
# 用 gcc-12 装 515.105.01
#
# 为什么需要这一步：
#   本机 kernel 6.1.0-50 用 gcc-12 编译；但系统默认 gcc 已被升到 14.2.0。
#   GCC 14 把 "implicit declaration of function" 从警告升级为 **错误**，
#   而 515 的 nv-linux.h 引用了本内核头里没有的 dma_is_direct()，
#   于是 gcc-14 下直接编译失败；gcc-12 下只是一条警告，能编过。
#   （dma_is_direct 只出现在 procfs_nvswitch.o / i2c_nvswitch.o —— NVSwitch 专用，
#     P106-100 根本走不到这段代码，所以那条警告无害。）
#
#   证据：/proc/driver/nvidia/version 显示之前成功加载的模块是 "GCC version: gcc version 12.4.0"。
set -u
VER=515.105.01
RUN="<workdir>/drivers/$VER/NVIDIA-Linux-x86_64-$VER.run"
LOG=/tmp/switch_${VER}_gcc12.log
CC12=/usr/bin/gcc-12

exec > >(tee -a "$LOG") 2>&1
echo "======== 安装 $VER（kernel-cc=$CC12）开始 $(date '+%F %T') ========"
echo "内核: $(uname -r)"
echo "默认 gcc: $(gcc --version | head -1)"
echo "指定 gcc: $($CC12 --version | head -1)"
echo "当前驱动: $(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>&1)"

[ -x "$CC12" ] || { echo "!! 没有 $CC12"; exit 9; }
[ -f "$RUN" ]  || { echo "!! 找不到 $RUN"; exit 9; }

echo "--- 1) 停 sddm + 终止座位会话 ---"
systemctl stop sddm 2>/dev/null
sleep 1
for S in $(loginctl list-sessions --no-legend 2>/dev/null | awk '$4=="seat0"{print $1}'); do
    loginctl terminate-session "$S" 2>/dev/null && echo "  terminated session $S"
done
sleep 3

echo "--- 2) 清 GPU 持有者 ---"
pkill -9 -f heaven_x64 2>/dev/null
pkill -9 -f browser_x64 2>/dev/null
for i in 1 2 3 4 5; do
    H=$(fuser /dev/nvidia0 /dev/nvidiactl 2>/dev/null | tr -d ' \n')
    [ -z "$H" ] && { echo "  已无持有者"; break; }
    echo "  持有者 $H → 强杀"; fuser -k /dev/nvidia0 /dev/nvidiactl 2>/dev/null; sleep 2
done

echo "--- 3) 卸载模块 ---"
for i in 1 2 3 4 5; do
    modprobe -r nvidia_drm 2>/dev/null; modprobe -r nvidia_modeset 2>/dev/null; modprobe -r nvidia 2>/dev/null
    N=$(lsmod | grep -c '^nvidia'); echo "  剩余 $N"
    [ "$N" = "0" ] && break; sleep 2
done

echo "--- 4) 安装（强制 kernel-cc=gcc-12）---"
CC="$CC12" IGNORE_CC_MISMATCH=1 bash "$RUN" --silent --no-questions
RC=$?
echo "installer rc=$RC"
if [ "$RC" != "0" ]; then
    echo "!!!! 失败，日志尾部："; tail -25 /var/log/nvidia-installer.log
    echo "!!!! sddm 未启动"; exit "$RC"
fi

echo "--- 5) 加载 + 验证 ---"
modprobe nvidia 2>/dev/null; sleep 1
modprobe nvidia_drm 2>/dev/null; sleep 1
echo "版本: $(nvidia-smi --query-gpu=driver_version,name --format=csv,noheader 2>&1)"
echo "模块编译器: $(grep -i 'gcc version' /proc/driver/nvidia/version 2>&1)"

echo "--- 6) 起 sddm ---"
systemctl start sddm; sleep 4
echo "sddm = $(systemctl is-active sddm)"
echo "======== 结束 $(date '+%F %T') ========"
