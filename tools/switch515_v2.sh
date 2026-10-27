#!/bin/bash
# 切换到 515.105.01 —— v2
# v1 失败原因：systemctl stop sddm 只停登录界面，已登录的 KDE 会话（kwin_wayland/Xwayland/
# plasmashell）仍然活着并持有 /dev/nvidia*，导致 nvidia_drm 卸载不掉。
# v2 增加：终止图形会话 + 强杀仍持有 /dev/nvidia* 的进程，并逐轮确认模块引用计数归零。
RUN=<workdir>/drivers/515.105.01/NVIDIA-Linux-x86_64-515.105.01.run
LOG=/tmp/switch515_v2.log

exec > >(tee -a "$LOG") 2>&1
echo "================ 开始 $(date '+%F %T') ================"
echo "--- 当前驱动 ---"
nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>&1

echo "--- 1) 停 sddm ---"
systemctl stop sddm
sleep 1

echo "--- 2) 找到并终止图形会话 ---"
GRAPH_SESS=$(loginctl list-sessions --no-legend 2>/dev/null | awk '$4=="seat0"{print $1}' | head -1)
echo "图形会话 id = ${GRAPH_SESS:-未找到}"
if [ -n "$GRAPH_SESS" ]; then loginctl terminate-session "$GRAPH_SESS" 2>&1; fi
sleep 3

echo "--- 3) 杀残留 GPU 进程 ---"
pkill -9 -f heaven_x64 2>/dev/null
pkill -9 -f browser_x64 2>/dev/null
for i in 1 2 3 4 5; do
    HOLD=$(fuser /dev/nvidia0 /dev/nvidiactl 2>/dev/null | tr -s ' ')
    if [ -z "$(echo "$HOLD" | tr -d ' ')" ]; then echo "第 $i 轮：无进程持有 /dev/nvidia*"; break; fi
    echo "第 $i 轮：仍持有 =$HOLD  → 强杀"
    fuser -k /dev/nvidia0 /dev/nvidiactl 2>/dev/null
    sleep 2
done

echo "--- 4) 卸载内核模块（最多重试 5 次）---"
for i in 1 2 3 4 5; do
    modprobe -r nvidia_drm 2>/dev/null
    modprobe -r nvidia_modeset 2>/dev/null
    modprobe -r nvidia 2>/dev/null
    N=$(lsmod | grep -c '^nvidia')
    echo "第 $i 轮后剩余 nvidia 模块 = $N"
    [ "$N" = "0" ] && break
    sleep 2
done
lsmod | grep -E '^nvidia' || echo "（全部已卸载）"

echo "--- 5) 安装 515.105.01 ---"
bash "$RUN" --silent --no-questions
RC=$?
echo "installer rc=$RC"
if [ "$RC" != "0" ]; then
    echo "!!! 安装失败，日志尾部："
    tail -15 /var/log/nvidia-installer.log
    echo "!!! 不启动 sddm，保持可诊断状态"
    exit "$RC"
fi

echo "--- 6) 加载模块 ---"
modprobe nvidia 2>/dev/null; sleep 1
modprobe nvidia_drm 2>/dev/null; sleep 1

echo "--- 7) 验证 ---"
nvidia-smi --query-gpu=driver_version,name --format=csv,noheader 2>&1
head -1 /proc/driver/nvidia/version 2>&1

echo "--- 8) 起 sddm ---"
systemctl start sddm
sleep 4
echo "sddm = $(systemctl is-active sddm)"
echo "================ 结束 $(date '+%F %T') ================"
