# 报告 108 / N117：**找回当年的 PVE 配置了** —— 真机慢的真凶是"宿主 KWin + Xwayland"，不是 NVIDIA 驱动，也不是 vGPU

- 时间：2026-10-26 17:40
- ⛔ **2026-10-26 17:42 核心结论撤回（报告 109）**：本文 §2「真凶是宿主 KWin + Xwayland，guest 只有 1 层」**错**。
  用户指出：**PVE 虚拟机内用的也是 KDE，截图就是客户机，显示栈是直通进客户机的**。
  ⇒ guest 里同样是 `app → Xwayland → KWin → i915`，**层数与真机完全相同**。
  **§2 整节作废**，「零成本切 Plasma (X11)」那个实验也不再具有判别力（两边都是 KDE）。
  **本文仍然成立的部分**（全部是配置原文，与解释无关）：
  ① `100.conf` 全文；② `hostpci0 x-vga=1` / `hostpci1 mdev=nvidia-156` / `vga: none`；
  ③ 宿主 `blacklist i915` + `vfio-pci ids=8086:0c0c,8086:041a`（这条**仍然成立**：宿主确实没有显示栈，
     但那是因为**整块核显给了 guest**，不是因为"guest 少一层"）；
  ④ `profile_override.toml` 的 `framebuffer = 0x16f000000`（对上了截图 6016MiB）；
  ⑤ vGPU 机制链（550.144.02 + vgpu-proxmox patch + vgpu_unlock-rs → mdev nvidia-156）。
- 入口：用户给出 `<user>@<testhost>`（密码 `<user>`），指向 `/media/<user>/fix/`
- 性质：**一手配置复原**。所有结论下面都有配置文件原文，没有推理成分
- 证据落到：`analysis/pve-recovered/`（`config.db`、`100.conf`、`host-and-profile-config.txt`）

---

## 0. 这台机器上现在有三套系统

```
sda  (465.8G)  sda2 = ext4 "fix"        ← PVE 8（那套 vGPU 环境），已挂 /media/<user>/fix
sdb  (232.9G)  sdb2 = ext4 (hostname MSI) ← 真机 Debian 13 + 525.116.03，"慢"的那套
sdc  (476.9G)  sdc4 = /                 ← 当前启动的 "<testhost>"（没装 nvidia 驱动，nvidia-smi 不存在）
```

**同一块机器，两套并排放着。可以直接对配置，不用再猜。**

---

## 1. 复原出来的 VM 100 完整配置（`config.db` 里 `100.conf` 原文）

Proxmox 的 `config.db` 是 pmxcfs 的 sqlite 后端，`/etc/pve` 在离线时是空的，但库里全在。
解出来：

```yaml
args: -cpu 'host,enforce,+kvm_pv_unhalt,+kvm_pv_eoi,+vmx,+lahf_lm,+sep,hv_vendor_id=NV43FIX,kvm=off'
      -set device.hostpci3.x-igd-gms=0x4
balloon: 0
bios: ovmf
cores: 7
cpu: host,flags=+pcid
efidisk0: local:100/vm-100-disk-0.qcow2,efitype=4m,pre-enrolled-keys=1,size=528K
hostpci0: 0000:00:02,pcie=1,x-vga=1,romfile=P4600.rom          ← 核显：主 VGA 直通
hostpci1: 0000:01:00.0,mdev=nvidia-156,pcie=1                  ← P106：vGPU（mdev）
hostpci2: 0000:06:00.0
hostpci3: 0000:00:14.0
hostpci4: 0000:00:03
hostpci5: 0000:00:1b
hostpci6: 0000:00:16
machine: q35
memory: 14944
net0: virtio=BC:24:11:09:A5:24,bridge=vmbr0
ostype: l26
scsi0: /dev/disk/by-id/ata-WDC_WD5000AAKX-08ERMA0_WD-WCC2EC040621,size=488386584K   ← 整盘直通
scsi1: /dev/disk/by-id/ata-TOSHIBA_DT01ACA050_LENOVO_X7LGMJ9AS-part3
scsihw: virtio-scsi-single
vga: none                                                       ← 没有虚拟显卡
```

### 1.1 三条关键读出

| 配置 | 含义 |
|---|---|
| **`hostpci0: 0000:00:02, x-vga=1, romfile=P4600.rom`** | 核显**整块直通**，且 **`x-vga=1` = 让 guest 拿到这张卡的 VGA/主显示仲裁权**。guest 自己驱动物理 HDMI。 |
| **`hostpci1: 0000:01:00.0, mdev=nvidia-156`** | P106 **不是物理直通**，是 **mediated device（vGPU）**。VM 启动日志佐证：`-device vfio-pci,sysfsdev=/sys/bus/mdev/devices/00000001-0000-0000-0000-000000000100,id=hostpci1` |
| **`vga: none`** | 没有 QEMU 虚拟显卡，显示完全靠直通的核显 |

---

## 2. ★★★ 真正的差异：**PVE 宿主根本没有显示栈**

`/media/<user>/fix/etc/modprobe.d/` 原文：

```
--- vfio.conf ---
# options vfio-pci ids=10de:1c07              ← P106 物理直通方案，被注释掉了
options vfio-pci ids=8086:0c0c,8086:041a      ← 核显(041a) + 核显HDMI音频(0c0c) 绑到 vfio

--- blacklist.conf ---
blacklist nouveau
blacklist i915                                ← ★ 宿主不加载核显驱动
blacklist snd_hda_intel
blacklist snd_hda_codec_hdmi
```

**`blacklist i915` + `vfio-pci ids=8086:041a` ⇒ PVE 宿主不能输出、不能合成、不参与显示。**
宿主是一台纯无头机器。整块核显交给 guest，由 guest 的 Xorg 直接 KMS 打到物理 HDMI。

### 2.1 两套的显示路径并排

```
【PVE guest（当年，快）】
  app (heaven_x64)
    → guest 的 Xorg（唯一一层 X server）
    → guest 独占的 i915（x-vga=1 直通）
    → 物理 HDMI
  共 1 个进程、0 个合成器转发、0 个宿主参与

【真机 MSI（现在，慢）】
  app (heaven_x64)
    → Xwayland（嵌套 X server）
    → 宿主 KWin 6.3.6（Wayland 合成器，每帧合成）
    → 宿主 i915
    → 物理 HDMI
  共 2 个服务器、每帧一次合成、宿主全程参与
```

**⇒ 真机比 guest 多了一整层：Xwayland + Wayland 合成器。**

这一条**与 NVIDIA 驱动无关、与 vGPU 无关、与 525/515 无关**，纯粹是显示栈结构差异。
之前 100 多份报告全在 NVIDIA 的库里找原因，而真正的差异在**显示服务器和合成器**这一层。

### 2.2 为什么这能解释"真机受限"

| 机制 | 说明 |
|---|---|
| **多一次 present 中转** | app 的 present 先到 Xwayland，再由 KWin 合成，最后才 flip。我们实测卡在 `poll → xcb_wait_for_special_event` 等 CompleteNotify —— **这个 CompleteNotify 是 KWin 在合成完 + flip 完后才发的** |
| **被合成器节拍量化** | 报告 106 实测：NVIDIA 尺寸扫描 64×64 能到 90fps（11.1ms）但 800×600 掉到 61 —— 因为大帧的合成/拷贝成本撞上 KWin 的 vblank 节拍 |
| **Intel 那一侧也在忙** | KWin 用自己的 i915 做合成，NVIDIA 的 DMA-BUF 只是它的一个输入源 |

**guest 里没有这一层**：Xorg 直接 KMS，present 就是 flip，完成事件由 drm 在 flip 后立即发。

---

## 3. vGPU 那部分：`GRID P40-2B` 的真相（与截图对上）

`/etc/vgpu_unlock/profile_override.toml` 原文：

```toml
[vm.100]
frl_enabled = 0
cuda_enabled = 1
num_displays = 1
display_width = 2560
display_height = 1440
max_pixels = 3686400
framebuffer = 0x16f000000        ← 6,164,152,320 B = 5.74 GiB
```

**`framebuffer = 0x16f000000` ≈ 6.16 GB ⇒ 这就是截图里 `646.0MiB / 6016MiB` 的来源。**

⇒ `GRID P40-2B` 只是**借来的 type 名**（P40 的 2GB 图形型），实际被 override 成**整卡 6GB**。
⇒ 报告 107 的结论（"不能用这个名字反推配置"）被配置原文确认。

历史版本也在，能看出演进过程：

```
profile_override.toml.bak:  [profile.nvidia-156] pci_id = 0x1BB010DE  pci_device_id = 0x1C03
                            ↑ 0x1C03 = GTX 1060 6GB，早期是伪装成 1060
profile_override.toml.Q:    [vm.100] vgpu_type = "NVS"    framebuffer = 0x16a000000
```

### 3.1 vGPU 机制链（配置+日志双证）

```
nvidia-550.144.02（宿主 vGPU 驱动，含 nvidia-vgpud / nvidia-vgpu-mgr / libnvidia-vgpu.so）
  + /opt/vgpu-proxmox/550.144.02.patch
      "This patch also restores vGPU capability on Pascal generation and older GPUs"
  + /opt/vgpu_unlock-rs/target/release/libvgpu_unlock_rs.so（LD_PRELOAD 进 vgpud/vgpu-mgr）
  ⇒ 造出 mdev type nvidia-156
  ⇒ qemu -device vfio-pci,sysfsdev=/sys/bus/mdev/devices/...
  ⇒ guest 看到 "GRID P40-2B"，framebuffer 被 override 到整卡
```

---

## 4. ★ 一个零成本的判决实验（现在就能做）

**真机的 MSI 系统里 `plasmax11.desktop` 是存在的**（`/mnt/sdb2/usr/share/xsessions/`）。

**在登录界面把会话类型从 "Plasma (Wayland)" 换成 "Plasma (X11)"**，然后重跑 Heaven / glxgears / vkmark。

切换后的路径变成：

```
app → Xorg（KWin X11 合成）→ i915 → HDMI
```

**比 Wayland 少一层 Xwayland 嵌套**，和 guest 的结构更接近（guest 也是 Xorg 单层）。

| 结果 | 判读 |
|---|---|
| **帧率明显上升**（比如 30 → 60+） | **瓶颈是 Xwayland/Wayland 这一层**，与 NVIDIA 驱动、与 vGPU 都无关 ⇒ 以后就用 X11 会话，案子转向显示栈 |
| **仍是 30fps** | 显示栈不是主因 ⇒ 回到提交层（118.5µs/对）和 vGPU 假说 |

**进一步（更彻底）**：在 X11 会话里关掉 KWin 合成（`kwin_x11 --replace` 关 compositing，或换一个纯窗口管理器如 openbox），
路径就变成 `app → Xorg → i915`，**与 guest 完全同构**。这是最强的对照。

---

## 5. 这份配置解答/修正了哪些旧结论

| 旧结论 | 现在 |
|---|---|
| 报告 94/105/106 说"用户截图 130W" | ⛔ 已撤回（报告 107），本报告再次确认：图上 `Pwr` 是 `N/A` |
| 报告 106 §4 "vGPU 有虚拟 CRTC、present 立即返回" | ⛔ 已撤回（报告 107）；**本报告给出真因**：不是 vGPU 的功劳，是**宿主没有显示栈** |
| 报告 106 "P106 不是显示拥有者" | ✅ **仍然成立，而且现在知道 guest 里也一样** —— guest 里当显示拥有者的是**直通进去的核显**，不是 P106 |
| "真机与 vGPU 的差异只能在提交层（118.5µs）" | ⚠️ **需要新增一条**：还有**显示栈层**（宿主 KWin+Xwayland vs guest 单层 Xorg），而且这一条**可零成本验证** |
| 报告 99 "P106 必须伪装成 P40" | ✅ 确认，且补上完整机制：mdev + patch + vgpu_unlock-rs + profile override |
| 报告 107 "6016MiB 说明参数被 override" | ✅ **配置原文确认**：`framebuffer = 0x16f000000` |

---

## 6. 一句话

**找回配置后，答案和我之前所有猜测都不一样：**
**PVE 宿主 `blacklist i915` + 把核显 vfio 绑走（`x-vga=1` 直通给 guest），宿主根本没有显示栈；guest 里 app → Xorg → i915 一跳直达 HDMI。**
**真机上是 app → Xwayland → 宿主 KWin → i915，多一层服务器 + 每帧合成。**
**P106 在 guest 里是 vGPU（mdev=nvidia-156），profile 被 override 到整卡 6.16GB（对上截图 6016MiB）。**
**所以"真机受限"最可能的原因不是 NVIDIA 驱动，而是宿主多了一台 Wayland 合成器 —— 而这一条，在登录界面切到 Plasma (X11) 就能验。**
