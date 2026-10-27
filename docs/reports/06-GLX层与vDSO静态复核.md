# 报告 06 — GLX 层静态复核 + vDSO 自旋证据固化（2026-10-24 夜）

- **输入**：用户要求"先解压 + 查看推进"，并明确**除定位问题点外不建议过多上机测试** → 本轮全部为静态/离线分析，零上机操作。
- **本轮唯一上机相关动作**：解压 `analysis/onmachine/p106-dump.zip`（NvDebugDump 产物，6 个文件）到 `analysis/onmachine/p106-dump/`。

---

## 1. 解压产物检查（p106-dump.zip）

| 文件 | 大小 | 内容 |
|---|---|---|
| `system_info.pb` | 4256B | protobuf，系统/驱动信息 |
| `nvlog.log` / `nvlog.gpu000.log` | 527552B ×2 | NvLog 二进制，**非明文**（magic `3dce4490…`，strings 为空） |
| `rm_00.pb` | 18880B | RM 状态 protobuf |
| `error_data.pb` | 320B | — |
| `debug_buffers_00.pb` | 16B | — |

- `nvlog` 试过 zlib/gzip（含跳过头部 0-15 字节、wbits 15/-15/31）**均无法解压** → NVIDIA 私有 NvLog 分帧格式，需官方解析器；本轮不作为证据来源。
- `system_info.pb` / `rm_00.pb` 为 protobuf，字段名未知，暂未解析（低优先级）。

---

## 2. Ghidra / ghidrecomp 环境打通（本轮关键基础设施成果）

用户提供的用法在实际环境有 **两个坑**，已定位并解决：

| 坑 | 现象 | 解法 |
|---|---|---|
| `GHIDRA_INSTALL_DIR` 未设置 | `ValueError: GHIDRA_INSTALL_DIR is not set` | `export GHIDRA_INSTALL_DIR=/data/software/apps/ghidra/main/x86_64/decomps/ghidra_12.1.4_PUBLIC` |
| **`/tmp` 是 10MB tmpfs** | Ghidra 保存工程时 `java.io.IOException: No space left on device`（磁盘其实有 15G 空闲，极具误导性） | **`JAVA_TOOL_OPTIONS` 生效，`JDK_JAVA_OPTIONS` 不生效**（pyghidra 用 jpype→`JNI_CreateJavaVM` 起 JVM，只有前者被 VM 读取）。用 `export JAVA_TOOL_OPTIONS="-Djava.io.tmpdir=<workdir>/analysis/ghidra/tmp"` 解决 |

正确用法（已验证可跑通）：

```bash
source /home/<user>/.pyenv/versions/pri_esc/bin/activate
export GHIDRA_INSTALL_DIR=/data/software/apps/ghidra/main/x86_64/decomps/ghidra_12.1.4_PUBLIC
export JAVA_TOOL_OPTIONS="-Djava.io.tmpdir=<workdir>/analysis/ghidra/tmp"   # 必须，否则 ENOSPC
cd <workdir>/analysis/ghidra
ghidrecomp <二进制> --filter <符号名> --callgraphs -o outXXX
# 全量反编译：去掉 --filter（1.2MB 的 libGLX_nvidia 约 20s/4300 函数）
```

> 产物：`analysis/ghidra/out515/`、`out525/`（两个版本的 libGLX_nvidia.so 全函数反编译 C 代码）。

---

## 3. GLX 层结论：**两代之间结构完全一致**（重要的负面结果）

对 `libGLX_nvidia.so` 515.105.01 与 525.116.03 做了 objdump + Ghidra 双重对比：

| 检查项 | 515.105.01 | 525.116.03 | 判定 |
|---|---|---|---|
| `glXSwapBuffers` 指令数 | 124 | 132 | 同一骨架 |
| 调用的 glcore 接口槽 | 0x598 / 0x5a0 / 0x5a8 / 0x5b0 / 0x5b8 | 完全相同 | **无变化** |
| 内部 swap helper（A 路径） | FUN@0x74b20，422 指令 | FUN@0x71890，397 指令 | 反编译 C **逐行等价**（仅 TLS 偏移 0x55efXX→0x8e0cXX、一次 4B→8B 拷贝差异） |
| 内部 swap helper（B 路径） | FUN@0x74fb0，128 指令 | FUN@0x71c90，129 指令 | 等价 |
| 导出符号 | 2674 | 2673（仅少 `glTexParametervNVX`） | 无实质变化 |
| `__GL_*` regkey | 62 个 | 62 个，**完全相同** | 无新增开关 |

**含义**：`glXSwapBuffers` 没有新增等待/节流逻辑；`__GL_SYNC_TO_VBLANK` / `__GL_MaxFramesAllowed` / `__GL_YIELD` 这些可能控制帧节流的 regkey 在两代都存在且同名。
→ **改动 100% 在 `libnvidia-glcore.so`（36MB→39MB）内部**，即接口槽 0x598/0x5a0/0x5a8/0x5b0/0x5b8 背后函数的实现。

---

## 4. perf 自旋归属的证据固化：glcore 确实自带 vDSO clock_gettime

此前疑点：`nm -D --undefined-only` 显示 glcore **不导入** `clock_gettime`，怀疑报告 05 的 perf 归属（"33% CPU 在 clock_gettime，调用方 glcore"）是缺帧指针导致的栈展开误差。

**本轮证伪该疑点——perf 归属是对的**：

- glcore 二进制内含 `linux-vdso.so` / `[vdso]` / `clock_gettime` / `gettimeofday` 四组字符串；
- 515 中可完整读出包装函数（`glcore+0xeb2c40`）：

```c
// glcore 515: 自建 vDSO clock_gettime（dlvsym 优先，syscall(228) 回退）
fn = dlvsym(0, "linux-vdso.so", "clock_gettime", "LINUX_2.6");  // 存全局 0x2525b00
if (fn) return fn(clk_id, tp);
else    return syscall(228 /* clock_gettime */, clk_id, tp);
```

→ 驱动绕过 libc PLT 直接打 vDSO，所以**不出现导入符号**，但 perf 会把 vDSO 帧解析成 `clock_gettime` 并正确归属到调用方 glcore。
→ **"3D 呈现在驱动侧自旋等时间戳"这一核心观测成立**，不是栈展开假象。

---

## 5. glcore 侧新发现（候选根因面）

| 版本 | glcore 新增（与 present/节流相关） |
|---|---|
| 525.116.03 | `VK_NV_present_barrier` 及整套 "present barrier" 错误串（QuadroSync 多卡帧同步）；`glpmSampleNV` / `glpmTriggerNV` / `glpmWaitForIdleNV` 三个新入口点（位于 GL 函数名表区域，515 无、580 也无 → 525 代短暂存在）；`ForceGLXTrueColorVisual` |
| 580.178.04 | `Failed to update drawable's present mode (%d)`、`Failed to activate present barrier on GPU` 等 |

- 说明 **present 路径在 520/525 一代确实被重构过**（与报告 01 的版本边界互相印证）。
- `glpm*NV` 三件套（Sample/Trigger/WaitForIdle）形态上是"采样-触发-等空闲"的同步原语，525 代引入、580 代消失，疑似内部 API 改名/重构，**尚未确认是否与 GL 呈现代码相关**（待 Ghidra 定位其调用点）。

---

## 6. 仍未打通的一环（诚实记录）

要把 5 个接口槽（0x598/0x5a0/0x5a8/0x5b0/0x5b8）映射到 glcore 具体函数，需要找到"接口结构体由谁注册"：

- GLX 侧全局指针（515: `0x33bce8`，525: `0x124d08`）位于 `.bss`，**无重定位**；
- GLX 自身代码中该地址的 198 处引用**全部是 `lea`（取址后读取）**，扫描基本块未找到任何写入指令；
- glcore 的导入表只有 glibc + `_nvNNNtls`，**不从 GLX 导入任何符号** → 无法静态确定注册路径。

**下一步可行路径**：对 glcore 做全量 Ghidra 分析后用 Reference Manager 反查（工程已可跑，但 39MB 全量分析耗时/耗内存较大，建议夜间后台跑）；或改从运行时侧取签名（`perf` 带符号 / `ltrace` 一次即可拿到槽位函数的真实地址）。

---

## 7. 结论与建议的下一步

**对用户问题的回答**（详见 README"核心结论"）：P106 的"3D 功耗被限制在 60-70W"**不是功耗墙**，而是 3D 呈现路径被限帧（~30fps）的次生现象——同卡同驱动下 glmark2 能到 79W、CUDA 能跑满，Heaven 全程 `SwPowerCap=Not Active`、时钟满频、功耗却只有 42-50W。真正被限制的是**帧率**，功耗数字只是"GPU 没干活"的结果。

**按"最小上机"排序的下一步**：

| 优先级 | 动作 | 上机成本 | 预期判别力 |
|---|---|---|---|
| **P0** | 只加环境变量重跑 Heaven：`__GL_SYNC_TO_VBLANK=0 __GL_MaxFramesAllowed=4`（再试 `=1`）、`__GL_YIELD=NOTHING` | **极低**（不装驱动、不重启、不改系统） | 若帧率解封 → 直接锁定"帧队列/呈现节流"；若无效 → 排除该面 |
| **P1** | 纯 Xorg 会话（不经 Wayland/Xwayland）跑同一 Heaven | 低（切会话） | 分离"驱动节流" vs "Xwayland 合成交互"两个竞争假说 |
| **P2** | glcore 全量 Ghidra 分析 + 接口槽反查 | 零（纯静态，耗时） | 静态 pinpoint 到函数级 |
| **P3** | `lspci -vv -s 01:00.0`（只读）量化 PCIe 链路（已知 x16 / 2.5GT/s = Gen1） | 零 | 排除/确认 PCIe Gen1 带宽-延迟假说 |

---

## 8. 本轮产出文件

- `analysis/onmachine/p106-dump/`（解压产物）
- `analysis/glcore/`：`full-{515,525,580}.asm`（全量反汇编，各 4-6.8M 行）、符号/字符串/regkey diff 产物
- `analysis/glx/`：`glx-{515,525,580}.asm`、`swap-*.txt`、`helper-*.txt`
- `analysis/ghidra/out515/`、`out525/`：libGLX_nvidia 两代全函数 Ghidra 反编译 C 代码
- `analysis/ghidra/find_writers.py`：Ghidra 引用反查脚本（可复用）
- 本报告
