# 19 · Ghidra 反编译 + 运行时 vtable 解析：hop4 的唯一差异是「解锁定属主校验」，已定量排除

> 2026-10-25 13:40
> 目的：报告 18 用汇编做到"热点函数逐行等价"后，还剩一对增长最大的函数（+471B / +156B）身份不明。
> 用户腾出 57G 空间后，Ghidra 全量路线解禁，本文用反编译 + 运行时 vtable 解析把它彻底解开。
> **结论：+471B 是 525 新增的"解锁定属主校验"（`pthread_equal(self, owner)`），是正确性修复，代价 ~ns 级，不可能造成 16.67ms 地板。hop4 正式出局。**

---

## 1. Ghidra 环境：三个坑（都已固化到脚本）

| 坑 | 现象 | 解法 |
|---|---|---|
| **`/dev/fd` 不存在** | `launch.sh: 行 179: /dev/fd/63: 没有那个文件或目录` → `ClassNotFoundException: Ghidra class loader not in use` | 沙箱 `/dev` 是每进程独立的 tmpfs，符号链接不持久。**必须在同一 Bash 进程内** `ln -sf /proc/self/fd /dev/fd` 再启动 |
| **`java.io.tmpdir` 在 10MB tmpfs** | 导入时报 `java.io.IOException: 设备上没有空间`（工程目录明明有 57G） | `export TMPDIR=<大分区> JAVA_TOOL_OPTIONS="-Djava.io.tmpdir=<大分区> -Xmx6g"`（报告 06 已记录，本次复现） |
| **imageBase = 0x100000** | 在 objdump vaddr `0xfbe5b0` 建函数，反编译出**完全无关的代码**（访问 `param_1+0x14`，而真实 drain 访问 `+0x54d/+0x6f88`） | Ghidra 对 ELF 设 `imageBase=0x100000`，**Ghidra 地址 = objdump vaddr + 0x100000**。脚本内必须用 `currentProgram.getImageBase().add(vaddr)` |

> 第三个坑最危险：它**不报错，静默给出错误函数的反编译**。第一次就踩了——反编译出来的"drain"里有 `iVar1 == 2/3/1` 分支和 `func_0x00369440`，实际是 vaddr `0xebe5b0` 处的另一个函数。
> **校验方法**：反编译结果必须能对上已知的汇编特征（如 `+0x54d & 2`、`imul $0xfa`、魔数 `/1000`）。

脚本：`analysis/ghidra/scripts/DecompRange.java`（定向反编译单区间，自动处理 imageBase）

```bash
ln -sf /proc/self/fd /dev/fd
export TMPDIR=$GD/tmp JAVA_TOOL_OPTIONS="-Djava.io.tmpdir=$GD/tmp -Xmx6g"
analyzeHeadless $GD/gc515 gc515 -import <so> -noanalysis \
  -postScript DecompRange.java fbe5b0 fbe6bf drain515 $GD/d515_drain.c -scriptPath $GD/scripts
```

用 `-noanalysis` + 定向 `DisassembleCommand`，36MB/44MB 的 stripped 库**几分钟**出结果，不必全量分析（磁盘 92% 时也扛得住）。

---

## 2. drain 函数：反编译确认 `deadline = now + 250×(N−1)`

`d515_drain.c`（1149 字符）核心：

```c
if ((param_1 != 0) && ((*(byte *)(param_1 + 0x54d) & 2) != 0)) {
    uVar2 = func_0x010be540();
    if (((bRam_0x261f840 & 7) == 0) &&
        ((*(long *)(param_1 + 0x1258) == 0 || ((*(uint *)(*(long*)(param_1+0x1258) + 0xc) & 0x88000) == 0)) &&
         (*(char *)(param_1 + 0x6f88) != '\0')) && (*(int *)(param_1 + 0x22e0c) != 0)) {
      lVar3 = (*(code *)_nv014glcore._640_8_)();
      lVar3 = lVar3 / 1000;                                    // ← ns → µs
      if (*(long *)(param_1 + 0x6f90) == 0) {
        *(undefined1 *)(param_1 + 0x6f89) = 0;
        *(ulong *)(param_1 + 0x6f90) = lVar3 + (ulong)(*(int *)(param_1 + 0x22e0c) * 0xfa - 0xfa);
      }                                                        // ← now + 250*(N-1)
      else if ((*(long *)(param_1 + 0x6f90) < lVar3) && (*(char *)(param_1 + 0x1510) == '\0')) {
        bVar1 = *(char *)(param_1 + 0x6f89) + 1;
        if (bVar1 < 4) { ... 重设 deadline ... } else { uVar2 = 1; }   // ← 最多 3 次
      }
    }
}
```

**`250×(N−1)` 从"反汇编常数推断"升级为"反编译确认"**，且 `/1000` 确认为 ns→µs。

525 版（`d525_drain.c`，1236 字符）**逐句对应**，仅偏移漂移：

| 语义 | 515 | 525 |
|---|---|---|
| 门1 标志 | +0x54d & 2 | +0x5ad & 2 |
| 全局门 | `0x261f840 & 7` | `0x2b61e40 & 7` |
| 对象 | +0x1258 | +0x12b8 |
| 门2 | +0x6f88 | +0x6fc0 |
| N 乘数 | +0x22e0c | +0x22edc |
| **读时钟槽** | `_nv014glcore._640_8_` (0x280) | `_nv014glcore._656_8_` (0x290) |
| deadline / 重试 / 旁路 | +0x6f90 / +0x6f89 / +0x1510 | +0x6fc8 / +0x6fc1 / +0x1538 |

> `_nv014glcore` 表的**槽位整体 +0x10**（0x280→0x290），说明该表跨版本重排。

---

## 3. hop4：控制流同构，差异只有一处

`hop4` 是个 4 参数函数（`param_1` ctx、`param_2` 数组容器、`param_3` 类型码、`param_4` 标志），
内含：全局计数、`_nv017glcore` 的 lock/unlock、`_nv018glcore._0_8_(n*8)` 分配、遍历 `param_2+0xa8` 的数组拷贝、
遍历 `lRam_0x262d4a8` 的**多上下文（多 GPU）**列表，并按 `param_3 == 8 || param_3 == 2` 走不同分支。
——典型的**命令/资源批处理提交器**，且**在多 GPU 场景才走复杂路径**（`uRam_0x262d49c` = 上下文数）。

### 3.1 结构统计（`analysis/probes/cdiff.py`）

| 指标 | 515 | 525 |
|---|---|---|
| C 行数 | 435 | 462 |
| `if` | 82 | 86 |
| `while` / `do` | 11 / 10 | 10 / 10 |
| `goto` / `return` | 22 / 5 | 20 / 5 |
| 直接调用目标数 | 9 | 9 |
| `param_1` 字段数 | 9 | 9 |

**`param_1` 的 9 个偏移一一对应，但漂移量不统一**：前 6 个 +0x60，后 3 个 +0x38
→ ctx 结构体中段**收缩了 0x28 字节**（不是简单追加）。

### 3.2 唯一实质差异：解锁定属主校验

**515**（`param_1 == 0` 分支）：
```c
if (0 < (int)_nv019glcore._0_4_) {
    _nv019glcore._0_4_ = _nv019glcore._0_4_ + -1;
    _nv019glcore._48_4_ = _nv019glcore._48_4_ + -1;        // 无条件递减
    if (_nv019glcore._48_4_ == 0) { _56_4_ &= 0xffffff00; _64_8_ = 0; }
    (*(code *)_nv017glcore._24_8_)(...);                    // 无条件解锁
}
```

**525**：
```c
if (_nv019glcore._0_4_ != 0) {
    _nv019glcore._0_4_ = _nv019glcore._0_4_ + -1;
    auVar17 = (*(code *)_nv022glcore._0_8_)();                       // ← 新增调用 A
    cVar6 = (*(code *)uVar11)(auVar17._0_8_, auVar17._8_8_,
                              _nv019glcore._56_4_, _nv019glcore._64_8_);  // ← 新增调用 B
    if ((cVar6 != '\0') && (_nv019glcore._48_4_ != 0)) {             // ← 条件化！
        _nv019glcore._48_4_ = _nv019glcore._48_4_ + -1;
        if (_nv019glcore._48_4_ == 0) { _56_4_ &= 0xffffff00; _64_8_ = 0; }
        (*(code *)_nv017glcore._24_8_)(...);                         // 仅校验通过才解锁
    }
}
```

这个模式在 525 的 hop4 里出现 **10 次**（每个解锁路径前一次）。515 里 **0 次**。

| 统计 | 515 hop4 | 525 hop4 |
|---|---|---|
| `_nv022glcore._0_8_` 调用 | 4 | 9 |
| `_nv022glcore._8_8_` 调用 | **0** | **10** |

> 注意：515 全库其他函数里 `call *0x8(%r)` 有 16 处，所以**能力两版都有**，只是 525 把它加进了这个提交函数。

---

## 4. 运行时 vtable 解析：`pthread_self` + `pthread_equal`

`_nv022glcore` 位于 `.bss`（515 基址 0x252c1c0，在 0x2510e80–0x2539a68 内），**静态内容全 0**——运行时构造。
用 `gdb -p` 读 515 进程（heaven + primus）里的实际值：

| 槽 | vaddr | 所属 | 内容 |
|---|---|---|---|
| `_nv022glcore+0x0` | `0xeb7b40` | glcore 内 | 包装函数 A |
| `_nv022glcore+0x8` | `0xeb7b60` | glcore 内 | 包装函数 B（**515 里也有效！**） |
| `_nv022glcore+0x10` | — | — | 0 |

包装函数 A（`0xeb7b40`，26 字节）：
```asm
lea 0x166e0fd(%rip),%rax   # 2525c48
call *(%rax)                # → libc pthread_self
mov %rax,%rdx
mov $0x1,%eax               # 返回 (1, pthread_self())
ret
```

包装函数 B（`0xeb7b60`，55 字节）：
```asm
test %dil,%dil              # 参数1
jne  .else
  xor $0x1,%eax; ret        # 参数1==0 → 返回 !g56
.else:
  test %dl,%dl
  je   .ret0                # g56==0 → 返回 0
  lea 0x166e0c1(%rip),%rax  # 2525c40
  mov %rsi,%r8 ; mov %r8,%rdi ; mov %rcx,%rsi
  call *(%rax)              # → libc pthread_equal(b, g64)
  test %eax,%eax ; setne %al
```

再解一层（同一次 gdb，`/proc/pid/maps` 归属）：

| 槽 | libc 偏移 | 反汇编 | 函数 |
|---|---|---|---|
| `0x2525c40` | 0x93a50 | `xor eax,eax; cmp %rsi,%rdi; sete %al; ret` | **`pthread_equal`** |
| `0x2525c48` | 0x98f00 | `mov %fs:0x10,%rax; ret` | **`pthread_self`** |

### 4.1 完整语义

```c
// 包装 A  = 取自身线程 ID
pthread_self()  →  return (1, self);

// 包装 B  = 属主校验
bool check(a, b, g56, g64):
    if (a == 0)      return !g56;
    if (g56 == 0)    return false;
    return pthread_equal(b, g64);
```

调用方 525 传入 `a=1`（包装 A 的首值恒为 1），所以恒走 else：
```c
if (pthread_equal(pthread_self(), g64) && g48 != 0) { ... 递减 + 解锁 ... }
```

**`g56` / `g64` 就是锁的持有者记录**（`g56`=占用标志，`g64`=持有者线程 ID）。

→ **525 把"无条件解锁"改成"仅当自己是锁的属主才解锁"。这是典型的 owner-checking mutex 修复，用于防止跨线程误解锁。**

---

## 5. 定量排除：这个改动不可能造成 16.67ms

| 操作 | 实际指令 | 成本 |
|---|---|---|
| `pthread_self` | `mov %fs:0x10,%rax` —— 一次 TLS 读 | ~1–2 ns |
| `pthread_equal` | `cmp` + `sete` —— 纯比较，无锁无系统调用 | ~1 ns |
| 2 次间接调用开销 | 2 × `call *(%rax)` | ~2–4 ns |
| **每次校验合计** | | **~5–10 ns** |
| ×10 处/帧 | | **~50–100 ns/帧** |

对照：一个 vblank = **16.67 ms** = 16,670,000 ns。
**差值比是 5 个数量级。** 即便校验失败导致路径分叉（跳过解锁），那也只是"少做一次解锁"，不会产生毫秒级等待——
除非校验失败后走到了另一个含等待的路径，而反编译显示失败分支只是**跳过递减和解锁**，没有任何循环或睡眠。

**结论：hop4 的 +471 字节与 30fps 地板无关。hop4 正式出局。**

---

## 6. 本轮净收益

**排除了：**

1. hop4（`0xf1ea00`/`0x14d5dc0`，+471B）—— 差异是 owner-check，~ns 级
2. drain（`0xfbe5b0`/`0x1577a90`）—— 反编译逐句等价，`250×(N−1)` 与 `/1000` 确认
3. 同尺寸的 swap 派发器对（+156B）—— 控制流统计无新增循环（报告 18）

**新增能力（可复用）：**

- Ghidra 对 36/44MB stripped 库的**定向反编译**：`-noanalysis` + `DisassembleCommand` + `imageBase.add()`，几分钟出结果
- **`.bss` 运行时 vtable 解析**：`gdb -p` 读槽 + `/proc/pid/maps` 判归属 + `objdump` 认函数。
  这把"未知间接调用"变成"具体 libc 函数"，是本轮唯一能把间接调用链走到尽头的办法
- 三个 Ghidra 坑全部固化（§1）

**产物：**

```
analysis/ghidra/scripts/DecompRange.java   定向反编译（自动处理 imageBase）
analysis/ghidra/d515_drain.c               515 drain 反编译
analysis/ghidra/d525_drain.c               525 drain 反编译
analysis/ghidra/d515_hop4.c                515 hop4 反编译（435 行）
analysis/ghidra/d525_hop4.c                525 hop4 反编译（462 行）
analysis/ghidra/normdiff_hop4.txt          归一化 diff
analysis/probes/cdiff.py                   反编译 C 的结构特征对比
analysis/probes/normdiff.py                归一化 diff（偏移/调用/全局三元归一）
analysis/probes/readslots.sh               运行时读 .bss 槽
analysis/probes/mapof.sh                   槽值 → 所属库/函数
analysis/probes/vendortable.py             .data.rel.ro 重定位表读取
analysis/diffs/glcore-swap/vt0_515.func.asm 包装 A（pthread_self）
analysis/diffs/glcore-swap/vt1_515.func.asm 包装 B（pthread_equal）
```

---

## 7. 现在的处境：静态与反编译都已到尽头

三条最可疑的路径全部排除，剩下的可能必须靠**运行时归因**：

| 方向 | 为什么静态做不到 |
|---|---|
| 每帧到底在哪段时间溜走 | 需要 uprobe/perf 时间线对齐（报告 10 的 30.24ms 窗口仍未与帧间隔对齐） |
| present barrier 是否运行时激活 | 代码是配置期，是否参与取决于运行条件 |
| RM/内核交互（提交节流、vblank 归属） | 用户态库里看不到 |

**建议的下一步（都不需要装 525，可在 515 上建立基线）**：
在 primus 下同时抓 **`glXSwapBuffers` 时间戳** + **drain 被调用时间戳** + **锁 acquire/release 时间戳**，
把一帧切成"渲染 / 提交 / 等 vblank"三段，看 515 的 16.67ms 里各占多少。
这样等装回 525 时，同一张图直接对比就能定位那 16.67ms 落在哪一段——而不是继续在代码里找。
