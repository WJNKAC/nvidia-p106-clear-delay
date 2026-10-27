#!/bin/bash
# 通用驱动切换脚本：switchdrv.sh <版本号>   例：switchdrv.sh 525.116.03
# 流程取自报告 138 §1.1，并补上"终止图形会话 + 强杀 GPU 持有者"步骤（v1 缺这步导致 nvidia_drm 卸不掉）
set -u
VER="${1:?用法: switchdrv.sh <版本号，如 525.116.03>}"
RUN="<workdir>/drivers/$VER/NVIDIA-Linux-x86_64-$VER.run"
LOG="/tmp/switchdrv_$VER.log"

exec > >(tee -a "$LOG") 2>&1
echo "======== 目标 $VER  开始 $(date '+%F %T') ========"
echo "内核: $(uname -r)"
echo "头文件: $(ls -d /usr/src/linux-headers-$(uname -r) 2>/dev/null || echo 缺失)"
echo "当前驱动: $(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>&1)"

[ -f "$RUN" ] || { echo "!! 找不到 $RUN"; exit 9; }

echo "--- 1) 停 sddm ---"
systemctl stop sddm 2>/dev/null
sleep 1

echo "--- 2) 终止座位会话 ---"
for S in $(loginctl list-sessions --no-legend 2>/dev/null | awk '$4=="seat0"{print $1}'); do
    echo "  terminate-session $S"; loginctl terminate-session "$S" 2>/dev/null
done
sleep 3

echo "--- 3) 清 GPU 持有者 ---"
pkill -9 -f heaven_x64 2>/dev/null
pkill -9 -f browser_x64 2>/dev/null
for i in 1 2 3 4 5; do
    H=$(fuser /dev/nvidia0 /dev/nvidiactl 2>/dev/null | tr -d ' \n')
    [ -z "$H" ] && { echo "  第$i轮：已无持有者"; break; }
    echo "  第$i轮：持有者 $H → 强杀"; fuser -k /dev/nvidia0 /dev/nvidiactl 2>/dev/null; sleep 2
done

echo "--- 4) 卸载模块 ---"
for i in 1 2 3 4 5; do
    modprobe -r nvidia_drm 2>/dev/null; modprobe -r nvidia_modeset 2>/dev/null; modprobe -r nvidia 2>/dev/null
    N=$(lsmod | grep -c '^nvidia'); echo "  第$i轮：剩余 $N"
    [ "$N" = "0" ] && break; sleep 2
done

echo "--- 5) 安装 $VER ---"
CC=/usr/bin/gcc-12 IGNORE_CC_MISMATCH=1 bash "$RUN" --silent --no-questions
RC=$?
echo "installer rc=$RC"
if [ "$RC" != "0" ]; then
    echo "!!!! 安装失败，日志尾部："; tail -20 /var/log/nvidia-installer.log
    echo "!!!! sddm 未启动（保持可诊断）"; exit "$RC"
fi

echo "--- 6) 加载 + 验证 ---"
modprobe nvidia 2>/dev/null; sleep 1
modprobe nvidia_drm 2>/dev/null; sleep 1
echo "版本: $(nvidia-smi --query-gpu=driver_version,name --format=csv,noheader 2>&1)"
head -1 /proc/driver/nvidia/version 2>&1

echo "--- 7) 起 sddm ---"
systemctl start sddm; sleep 4
echo "sddm = $(systemctl is-active sddm)"
echo "======== 结束 $(date '+%F %T') ========"
