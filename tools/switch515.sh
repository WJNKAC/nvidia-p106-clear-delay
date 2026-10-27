#!/bin/bash
# 切换到 515.105.01（流程取自报告 138 §1.1，此前 5 次切换全部 exit=0）
RUN=<workdir>/drivers/515.105.01/NVIDIA-Linux-x86_64-515.105.01.run
LOG=<workdir>/analysis/switch515.log

exec > >(tee -a "$LOG") 2>&1
echo "================ 开始 $(date '+%F %T') ================"

echo "--- 1) 当前驱动 ---"
nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>&1

echo "--- 2) 停 sddm / 杀 X ---"
systemctl stop sddm
sleep 2
pkill -x Xorg 2>/dev/null; pkill -x Xwayland 2>/dev/null
sleep 2

echo "--- 3) 卸载内核模块 ---"
modprobe -r nvidia_drm 2>/dev/null
modprobe -r nvidia_modeset 2>/dev/null
modprobe -r nvidia 2>/dev/null
sleep 2
echo "剩余 nvidia 模块数: $(lsmod | grep -c '^nvidia')"

echo "--- 4) 安装 515.105.01 ---"
bash "$RUN" --silent --no-questions
RC=$?
echo "installer rc=$RC"

echo "--- 5) 安装日志尾部 ---"
tail -25 /var/log/nvidia-installer.log

echo "--- 6) 加载模块 ---"
modprobe nvidia 2>/dev/null
sleep 2
modprobe nvidia_drm 2>/dev/null
sleep 1

echo "--- 7) 验证 ---"
nvidia-smi --query-gpu=driver_version,name --format=csv,noheader 2>&1
cat /proc/driver/nvidia/version 2>&1 | head -1

echo "--- 8) 起 sddm ---"
systemctl start sddm
sleep 3
systemctl is-active sddm

echo "================ 结束 $(date '+%F %T') ================"
