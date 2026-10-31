#!/bin/bash
# 可靠基线：所有测量都用长帧数（让 GPU 稳定在满频）
# 判据：跑两遍，第二遍应与第一遍一致（说明已稳定）
NV=/etc/vulkan/icd.d/nvidia_icd.json
LVP=/usr/share/vulkan/icd.d/lvp_icd.json
OUT=/tmp/baseline2.txt
: > "$OUT"
XA=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)

sec() { echo >> "$OUT"; echo "===== $* =====" >> "$OUT"; }

sec "环境"
{
  echo "  时间   : $(date '+%F %T')"
  echo "  驱动   : $(nvidia-smi --query-gpu=driver_version --format=csv,noheader)  内核: $(uname -r)"
  echo "  链路   : $(lspci -vv -s 01:00.0 2>/dev/null | grep LnkSta: | sed 's/^\s*//;s/(.*//')"
} >> "$OUT"

sec "原生 GL（-s，长帧数 400，跑两遍看稳定性）"
for pass in 1 2; do
  echo "  第 $pass 遍:" >> "$OUT"
  for n in 100 400; do
    r=$(DISPLAY=:0 XAUTHORITY="$XA" __GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 \
        timeout 300 /tmp/batchprobe -n $n -f 400 -w 20 -s 2>&1 | grep per_batch)
    echo "    n=$n  $r" >> "$OUT"
  done
done

sec "zink（长帧数 400）"
for n in 100 400; do
  r=$(DISPLAY=:0 XAUTHORITY="$XA" MESA_LOADER_DRIVER_OVERRIDE=zink MESA_VK_DEVICE_SELECT=10de:1b87 \
      timeout 400 /tmp/batchprobe -n $n -f 400 -w 20 2>&1 | grep per_batch)
  echo "  n=$n  $r" >> "$OUT"
done

sec "原生 Vulkan（长帧数 400）"
for args in "-e|空提交(无render pass)" "-x|render pass 空操作" "-r 256|clear 256²" "-r 1024|clear 1024²"; do
  a="${args%%|*}"; d="${args##*|}"
  r=$(VK_ICD_FILENAMES=$NV timeout 400 /tmp/vkdraw -n 100 -d 0 -f 400 -w 20 $a 2>&1 | grep per_pass)
  echo "  $d  $r" >> "$OUT"
done

sec "Vulkan copy"
for n in 100 400; do
  r=$(VK_ICD_FILENAMES=$NV timeout 300 /tmp/vkprobe -n $n -d 1 -f 400 -w 20 2>&1 | grep per_submit)
  echo "  n=$n  $r" >> "$OUT"
done

sec "lavapipe（长帧数 400，纯 CPU 无升频问题）"
for args in "-e|空提交" "-x|render pass 空操作" "-r 256|clear 256²"; do
  a="${args%%|*}"; d="${args##*|}"
  r=$(VK_ICD_FILENAMES=$LVP VKPROBE_VID=0 VKPROBE_DID=0 timeout 400 /tmp/vkdraw -n 100 -d 0 -f 400 -w 20 $a 2>&1 | grep per_pass)
  echo "  $d  $r" >> "$OUT"
done

cat "$OUT"
