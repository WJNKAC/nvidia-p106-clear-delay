# N189 — 破局点验证：GDB 抓到等待点，lavapipe 对照证明那 ~144 µs 是 P104 特有

**日期**：2026-10-31
**驱动**：580.178.04 / kernel 6.12.113 / P104-100 / PCIe Gen1 x4
**状态**：只用户态清除延迟（内核白名单未打）
**问题**：N188 留下两个未解 —— 那 ~137 µs 到底在等什么？是不是受限 SKU？本轮按三个方向逐一推。

---

## 0. 结论

**拿到了三份硬证据，其中第三份是决定性的。**

### ① 主线程 88 % 的时间在 `poll()` 里等

gdb 抓到的完整调用链（从下往上读）：

```
#9  main()                                        ← 我们的代码
#8  0xc701de  eglcore
#7  0xd7d814  eglcore
#6  0xd7d386  eglcore        ← 每处理一个 pass 循环一次
#5  0xd81d99  eglcore
#4  0xa147e4  eglcore
#3  poll()                    ← 在这里等
```

`0xd7d2a4 .. 0xd7d386` 是个循环，每次迭代处理一个元素、提交一次、然后 poll 等它完成。

### ② 等的是 100+ 个 `/dev/nvidia0` 的事件

进程 fd 清单：**总共 130 个，其中 117 个是 `/dev/nvidia0`**。
`poll(nfds≈100, timeout=10ms)` 出现 4056 次，占主线程 poll 时间 99.9 %。

但**fd 数量不是原因**（已排除，见 §3）。

### ③ ★ 空的 render pass 在 P104 上要 154 µs，在 lavapipe 上只要 10 µs

| 配置（`-n 100`，每帧 100 次） | P104-100 | lavapipe（llvmpipe，纯 CPU） | 倍数 |
|---|---|---|---|
| 空提交（无 render pass） | 13.8 µs | 9.5 µs | 1.5× |
| **render pass 什么都不做**<br>（`LOAD=DONT_CARE` `STORE=DONT_CARE`，0 draw，不 clear） | **154.2 µs** | **10.0 µs** | **15.4×** |
| render pass + clear 256² | 153.2 µs | 62.8 µs | 2.4× |
| render pass + clear 1024² | 203.1 µs | 871.2 µs | 0.23×（GPU 反超） |

**这张表把三件事同时钉死了：**

1. **不是 Vulkan 通用开销** —— lavapipe 的空 render pass 只要 10.0 µs，和空提交（9.5 µs）几乎一样。正常实现里「进一次 render pass」是**免费的**。
2. **不是 GPU 工作量** —— P104 的「什么都不做」和「真的 clear 256²」一样贵（154.2 vs 153.2）。
3. **是 P104 特有的固定成本 ≈ 144 µs**（154.2 − 10.0），只要进 render pass 就付，与画什么无关。

**144 µs 与 GL 侧的 125 µs（147 → 21.8）是同一量级。**
⇒ 同一个受限 SKU 机制，两条用户态路径各有一份实现。

---

## 1. 方向一：GDB 抓现行

### 1.1 怎么抓的

`vkdraw -n 100 -f 3000` 跑起来（每帧 100 个 render pass，约 42 秒），`gdb -p` attach，`thread apply all bt`。

线程列表：

```
Thread 1  "vkdraw"            主线程  ← 在 poll
Thread 2  "vkdraw"                    futex
Thread 3  "[vkps] Update"             futex
Thread 4  "[vkrt] Analysis"           futex
Thread 5  "[vkcf] Analysis"           futex
Thread 6  "vkdraw"                    futex
```

`[vkcf]/[vkrt]/[vkps]` 三个线程——10-30 那次日世界（DXVK）卡死时也见过同样的三个。

### 1.2 调用链还原

用 maps 把绝对地址换算成文件偏移（eglcore 的 r-xp 段 `@0x242000`）：

```
c701de:  ... call 0xd7d780
         ────────────────
d7d2a4:  ← 循环起点
d7d2f1:  call 0xd7d120        (虚函数桩 jmp *0x48(%rax))
d7d301:  call 0xd7d110        (虚函数桩 jmp *0x18(%rax))
d7d32d:  call *0x290(%rbx)    ← 取时间戳
d7d33b:  cmp  $-999,%rcx      ← 时间预算检查
d7d365:  cmp  $0xa,%rdx       ← (时间差/100) >= 10 ?
d7d381:  call 0xd81d50        ← 提交
d7d386:  cmp 0x10(%rsp),%rax
d7d391:  jl   d7d2a4          ← 回到循环起点
         ────────────────
d81d50:  mov  0x2b8(%rdx),%rax    ← 从全局表 0x2180900 取函数指针
d81d97:  call *%rax
         ────────────────
a147d4:  ...
a147df:  call 0x2429c0            ← 真 poll
a147e4:  mov %eax,0x24(%rsp)      ← 返回点
```

**每个 pass：进循环 → 提交（0xd81d50）→ poll 等完成 → 检查时间预算 → 下一个。**

`0xd81d50` 通过全局表 `0x2180900 + 0x2b8` 取函数指针，说明真正的提交函数是**运行时分发**的
（所以静态搜索找不到固定调用点）。

### 1.3 strace 时间账

| 场景 | 主线程 poll 次数 | 其中 `(nfds≈100, tmo=10)` | 该 poll 平均耗时 |
|---|---|---|---|
| render pass ×100 | 4066 | 4056（99.9 %） | 216 µs |
| 空提交 ×100 | 486 | 少数 | — |

> ⚠ strace 会放大 syscall 开销，**绝对时间不可信**，只有次数与相对比例可靠。

---

## 2. 方向二：LD_PRELOAD 劫持 —— 先要判断值不值得

要劫持，先得知道 poll 是不是「假等」。测了 poll 本身的内核开销：

```
nfds=1    平均 0.4 µs
nfds=10   平均 0.6 µs
nfds=100  平均 2.8 µs      ← 100 个 fd 的扫描只要 2.8 µs
nfds=200  平均 5.5 µs
```

**poll 100 个 fd 只要 2.8 µs，而被观察到的等待是 216 µs（放大后）/ 144 µs（真实量级）。**

⇒ **不是 poll 的 O(n) 扫描成本，是真的在等事件。** 劫持 poll 加个超时并不能让事件来得更早；
要「绕过」就得伪造完成，那会破坏 fence 语义（数据竞态）。

**所以方向二在「不伪造完成」的前提下无效。** 除非伪造 —— 而伪造等于让 CPU 在 GPU 写完前读数据。

---

## 3. 顺带排除：fd 数量不是原因

| 配置 | `/dev/nvidia0` fd 数 | per_pass |
|---|---|---|
| `-n 10` | 27 | — |
| `-n 100` | 117 | 139 µs |
| `-n 200` | 217 | — |
| **`-n 1 -p 100`（1 次提交 100 pass）** | **18** | **139 µs（不变）** |
| **`-n 100 -e`（空提交）** | **117** | **7.5 µs** |

- fd 数 ≈ 30 + pass 数（每次 `vkBeginCommandBuffer` 开一个 `/dev/nvidia0`）
- 但 `-p 100` 只有 18 个 fd，per_pass 照样 139 µs
- 而空提交有 117 个 fd，只要 7.5 µs

⇒ **fd 多寡与快慢无关。** 差别只在命令缓冲里有没有 render pass。

---

## 4. 方向三：扫 eglcore 的「同构漏网之鱼」

上一轮（N188）已在全部 NVIDIA 用户态库里搜过那条命令的编码（`movabs $0xf020010e66` 与立即数）：

| 库 | movabs | 立即数 |
|---|---|---|
| `libnvidia-eglcore.so` | 2 | 2 |
| `libnvidia-glcore.so` | 1 | 2 |
| `libnvidia-vksc-core.so` | 0 | 2 |
| 其余 20+ 库 | 0 | 0 |

eglcore 里 `0xb7bcd1` 与 glcore 的 `0xb51c82` **结构完全同构**（字段 `0x1240`/`0x20b`/`0x80`/`0x88`
全一致，仅基址寄存器 r14→rbx），但把 `74`→`eb` 后 **139 → 138.9 µs，零变化**。

另一处 `0xd05160` 是**pushbuffer 空间检查**（`cmp 0x60(%rbx),0x68(%rbx)` + `je rel32`），不是标志位。

**本轮补充：也在内核模块里搜过** —— `nvidia.ko` 里**没有**完整的 `0x20010e66` 字面量
（`0x00000120` 有 121 处，多为误匹配）。
⇒ 那条命令不是内核直接构造的。

perf 的热点全部落在**虚函数派发桩**上（`d7d110: mov 0xf0(%rdi),%rdi; mov (%rdi),%rax; jmp *0x18(%rax)`），
间接跳转上 IP 会 skid，拿不到更深的位置。

---

## 5. 一个新发现：原本以为 lavapipe 不可用，是我的探针写错了

之前几轮一直报「lavapipe / Intel 枚举 0 设备」，以为是 Mesa 问题。
实际是**我自己的探针 bug**：`VKPROBE_VID=0 VKPROBE_DID=0` 时按 vendorID 匹配，而 lavapipe 的
vendorID 是 `0x10005`，永远匹配不上。

修法：vid/did 都为 0 时直接取第一个设备。修完 lavapipe 立刻可用。

> ⚠ 这个教训值得记：连续几轮把「自己的工具 bug」误判成「环境不支持」。
> 判断「工具链不可用」之前，先用第三方工具（`vulkaninfo`）交叉验证一次 ——
> 当时 `vulkaninfo --summary` 明明列出了 `llvmpipe`，我却只信自己的探针。

Intel 的 ANV 是真不可用：指定 `intel_icd.json` 后进程**核心转储**
（日志报 `MESA-INTEL: Haswell Vulkan support is incomplete`）。

---

## 6. 现在能说到什么程度

### 已被数据支持

| 命题 | 证据 |
|---|---|
| 等待发生在 `poll(/dev/nvidiaX)` | gdb 调用链 + strace 次数 |
| 不是 poll 内核开销 | 微基准：100 fd 只要 2.8 µs |
| 不是 fd 数量 | `-p 100` 只有 18 fd 仍 139 µs；空提交 117 fd 只 7.5 µs |
| **不是 GPU 工作量** | **空操作 render pass 与 clear 256² 一样贵（154 vs 153 µs）** |
| **不是 Vulkan 通用开销** | **lavapipe 空 render pass 只要 10.0 µs** |
| 是 P104 特有、与 GL 侧同量级 | 144 µs（Vulkan 附加）vs 125 µs（GL 补丁省下的） |
| 不在 glcore 补丁点、不在 eglcore 同构处 | 两处 `74→eb` 均已实测无效 |

### 仍不能说

1. **「一定是受限 SKU」尚未闭环。** 上面证明的是「P104 驱动多付 144 µs，且与工作量无关」。
   要把它和受限 SKU 直接连起来，唯一严格办法还是**打内核白名单做 A/B**（会中断图形会话，未做）。
2. **注入点未定位。** 只知道不在两处已找到的 `je` 上；函数是运行时分发的（全局表 +0x2b8），
   静态搜不到固定调用点。
3. lavapipe 的 10 µs 是**另一套实现**（Mesa 软件光栅），严格说对照的是「不同实现」而非
   「同一驱动的正常态」。但它足以排除「Vulkan 规范/loader 层固有开销」这个解释。

---

## 7. 对「纯用户态路线」的判断

三条方向逐一推完的结论：

| 方向 | 结果 |
|---|---|
| ① GDB 抓现行 | ✅ **成功**，拿到完整调用链与等待点 |
| ② LD_PRELOAD 劫持 | ❌ **不可行**——等的是真实事件，劫持 poll 只能靠伪造完成（数据竞态） |
| ③ 扫 eglcore 同构点 | ❌ **已穷尽**——两处 `je` 都实测无效，内核里也没有那条命令 |

**⇒ 纯用户态路线在 Vulkan 侧基本走不通。** 用户态能做的只有：
- 让应用少进 render pass（合批）—— 这是应用侧优化，不是驱动补丁
- 打内核白名单 —— 治本

这与 N186/N187 的结论一致，只是现在有了「空操作也要 154 µs」这个更硬的证据。

---

## 8. 下一步（唯一能闭环的实验）

打内核白名单做 A/B。预期：`-p 1 -r 256 -d 0`（空 render pass）从 154 µs 掉到 ~10 µs 量级。

```bash
cd <gen2 仓库>/drivers/580.178.04
sudo ./00-whitelist.sh apply
cd /usr/src/nvidia-580.178.04 && sudo make -j$(nproc)     # 必须 root
sudo <gen2 仓库>/drivers/580.178.04/sync-dkms.sh adopt    # 防 DKMS 覆盖
# 重载 nvidia 或重启
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json /tmp/vkdraw -n 100 -d 0 -r 256 -x
```

---

## 9. 复现

`tools/static/vkdraw.c` 现支持：

```
-n N   每帧提交次数
-p N   每次提交内 render pass 个数
-d N   每个 render pass 内 draw 数
-r N   分辨率
-e     空提交（命令缓冲里什么都不放）
-x     ★ 空 render pass（LOAD/STORE = DONT_CARE，0 draw，不 clear）
-s     每次提交后等 fence
```

关键三连（判「固定开销 vs 工作量」）：

```bash
NV=/etc/vulkan/icd.d/nvidia_icd.json
VK_ICD_FILENAMES=$NV ./vkdraw -n 100 -d 1 -f 20 -w 5 -e           # 无 render pass
VK_ICD_FILENAMES=$NV ./vkdraw -n 100 -d 0 -f 20 -w 5 -r 256 -x    # render pass 空操作
VK_ICD_FILENAMES=$NV ./vkdraw -n 100 -d 0 -f 20 -w 5 -r 256       # render pass + clear
# 前两个的差 = 纯 render pass 固定开销
```

对照用：

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
VKPROBE_VID=0 VKPROBE_DID=0 ./vkdraw -n 100 -d 0 -f 20 -w 5 -r 256 -x
```
