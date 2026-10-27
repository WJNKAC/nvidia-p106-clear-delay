# 33 · 每帧的 3ms 等的是「置位 + signal」那一下：已定位到指令级

> 2026-10-25 22:05
> 起点：用户指出"Unigine 的 1ms 轮询两版都有，不能解释差异"——正确，该线索作废。
> 本轮改做报告 32 的下一步：**抓唤醒方在 glcore 的具体代码位置**。
> **五条结论：**
> 1. **纠正线程角色**（gdb 栈）：`glcore+0x146d340`(wait_ms) 所在的线程才是"唤醒方"，primus worker 在 `usleep` 里。
> 2. **map 的 3ms 就是一次 `wait_ms`**：同一线程、100% 覆盖窗口、时长几乎完全相同（3037 vs 3039µs）。
> 3. **★ 唤醒它的是 tid 8838 在 `glcore+0x123af7f` 的 `call *0x2d8(%rbp)`（= signal）**，
>    而前一条指令是 `movl $0x1, 0x8e0d08(%r15)` —— **置完成标志**。这就是"完成通知"那一下。
> 4. **每帧恰好一次**：264 次 signal / 263 个 map 窗口。
> 5. **那次 3ms 长 wait 期间，全进程发生了 7~8 次 notify，没有一次命中它** → cond 路由有选择性，不是"通知不够"。

---

## 1. 先纠正线程角色（gdb 栈）

上一轮我从 ftrace 推断 worker=唤醒目标、waker=唤醒方，栈显示两者的**真实身份**：

```
LWP 7640（ftrace 里的"唤醒方"）：         LWP 7639（ftrace 里的"map worker"）：
  libc                                   libc
  pthread_cond_timedwait                 clock_nanosleep/nanosleep
  glcore+0x146d340   ← wait_ms          usleep
  glcore+0x123b31a   ← 命令队列处理器      primus/libGL.so.1+0x18895  ← 自适应睡眠
  glcore+0x14702fa   ← 线程入口
```

- **7640 是 glcore 的命令队列 worker**，卡在 `wait_ms(cond, 100ms)`
- **7639 是 primus 的 readback worker**，卡在 `usleep(sleep_usec)`

**所以"3ms"发生在 glcore 那条线程上，primus 只是等它的结果。**

---

## 2. map 的 3ms = 一次 `wait_ms`

把每次 map 窗口和 glcore 线程的 `wait_ms` 调用对齐（263 个窗口，2293 次 wait_ms）：

| 指标 | 值 |
|---|---|
| 每帧 `wait_ms` 次数 | **3 次**（244/263 窗口） |
| 窗口被 `wait_ms` 覆盖的比例 | **中位 101.8%**（几乎 100%） |
| **窗口内最长的那次 wait_ms** | **中位 3037 µs** |
| **它占窗口的比例** | **中位 100.0%** |
| 涉及线程 | **8490**（262/263 帧） |

map 中位 3039µs，那次 wait_ms 中位 3037µs —— **同一件事，误差 2µs。**

> `wait_ms` 的时长是**双峰**：65.5% 的调用 <0.5ms（快速返回），22.7% 是 20~29ms（长等待）。
> 每帧落在窗口里的那一次，正好是中位数 3ms 的那一档。

---

## 3. ★ 定位到指令级：`置标志 + signal`

对每次长 wait（≥1ms 共 718 次）往前找最近的一次 notify，记录它的调用者：

| 唤醒者 | 次数 | 说明 |
|---|---|---|
| **`glcore+0x123af85`** ← tid **8838** | **264** | **= 263 个 map 窗口，每帧一次** |
| `glcore+0x1238db9` ← tid 8854 | 260 | 另一条路径 |
| `glcore+0x1238db9` ← tid 8808（主线程） | 184 | 主线程也有 |

### 3.1 那段代码长什么样

```
123af66: mov   %rax, 0x8e0d00(%r15)     ; ctx+0x8e0d00 = 结果
123af6d: movl  $0x1, 0x8e0d08(%r15)     ; ★ ctx+0x8e0d08 = 1  ← 置完成标志
123af78: mov   0x8e0948(%r15), %rdi      ; rdi = 目标对象
123af7f: call  *0x2d8(%rbp)              ; ★ signal(rdi)      ← 返回地址 0x123af85
```

**这就是"完成通知"的发令点**：先置 `ctx+0x8e0d08 = 1`，再 signal。

> 注意 `call *0x2d8(%rbp)` 是**间接调用**，所以 gdb/提取工具都会把它归到"调用者的返回地址"上（`0x123af85` 是返回地址，不是函数起点）。这也是为什么 `extract_func` 报"不在任何 FDE 内"。

### 3.2 同步原语表的槽位（补齐）

`_nv028glcore` 这张 vtable 上：

| 槽 | 函数 |
|---|---|
| `+0x2d0` | **`wait_ms(cond, ms)`**（报告 23 定位，`0x123b314` 处调用） |
| `+0x2d8` | **`signal(obj)`**（本节，`0x123af7f` / `0x1238db3` 处调用） |
| `+0x110` | `broadcast`（notify 函数 `0x146c680` 内部用） |
| `+0x150` | `unlock` |
| `+0x160` | `lock` |

### 3.3 ctx 结构里的几个关键偏移

| 偏移 | 含义 | 出处 |
|---|---|---|
| `+0x8e0910` | signal 的目标（路径 A，`0x1238dac`） | 本节 |
| `+0x8e0948` | signal 的目标（路径 B，`0x123af78`） | 本节 |
| **`+0x8e0950`** | **wait_ms 等的 cond** | 报告 23（`123b309`） |
| `+0x8e0d00` | 结果/数据 | 本节 |
| **`+0x8e0d08`** | **完成标志（置 1）** | 本节 |
| `+0x8e0d24` | 等待计数 | 报告 23 |

---

## 4. 一个反直觉的观测：通知一点不稀缺

那次 **3ms 长 wait 期间，全进程发生了 7~8 次 notify**（分布：7 次有 136 例、8 次有 105 例），
**没有一次命中它**。

全进程 notify 频率：**1205 次/秒**（8 秒 9637 次），来自 4 个线程：

| 调用者 | 次数 |
|---|---|
| `glcore+0x1238db9` | 6781 |
| `glcore+0x123af85` | 2400 |
| `libGLX_nvidia+0x57a0e` | 263（每帧一次） |
| `glcore+0x1238dfd` | 173 |

**→ 所以问题不是"通知不够/被漏掉"，而是"这个 cond 有路由选择性，别人发的通知走不到它"。**

这否掉了"丢失唤醒（lost wakeup）"这一整类假设。

---

## 5. 现在的完整链条（已到指令级）

```
Heaven 提交命令
   ↓
tid 8838（glcore 命令队列处理器）处理某批命令
   ↓ 处理完：
   movl $1, ctx+0x8e0d08        ← 置完成标志
   call *0x2d8(%rbp)            ← signal（0x123af7f）
   ↓
tid 8490（另一 context 的命令队列处理器）
   在 wait_ms(ctx+0x8e0950 的 cond, 100ms) 里醒了
   等了 3037µs
   ↓
tid 8490 futex_wake → primus worker(8507)
   ↓
glMapBuffer 返回（mb_out）
   ↓
primus worker：usleep(28ms) → 下一轮
```

**每帧的 3ms 精确等于 tid 8490 在 `wait_ms` 里等 tid 8838 发那一次 signal 的时间。**

而 515 只要 1ms。差异只在"8838 从开始处理到发出那次 signal 花了多久"。

---

## 6. 下一步（很具体）

| 优先级 | 动作 | 能答什么 |
|---|---|---|
| **1（首选）** | **测 tid 8838 的执行节奏**：uprobe 在 `0x123af6d`（置标志）与它所在处理段的入口，看每次处理花多久；同时看它在这 3ms 里是否也在 `wait_ms` | 是"8838 自己在等别人"（链式）还是"8838 单纯算得慢" |
| **2** | **找 515 的对应代码**：在 515 的 glcore 里定位 `ctx 偏移 +0x8e0d08` 附近的"置 1 + call signal"序列，做静态 diff | 两版这段逻辑的差异（判据是偏移不同，需先定位 515 的 ctx 基址） |
| 3 | 若 1 显示链式等待，继续往上追一层（8838 等的是谁） | 链的长度；515 与 525 的链长差异 |
| 4 | 用 `ctx+0x8e0d24`（等待计数）看每帧实际等了几轮 | 直接数"轮次"，与 515 对照 |

> **一个可行的简化**：既然 `ctx+0x8e0d08` 是完成标志、`+0x8e0d24` 是等待计数，
> 那用 gdb 在 map 前后读这两个字段的值，就能直接得到"每帧转了几轮"。成本很低。

---

## 7. 本轮踩的坑

| 坑 | 症状 | 修法 |
|---|---|---|
| **uprobe 的 fetch 语法在本机不可靠** | `ms=%esi:u32` 让整条探针注册失败，且 `cat uprobe_events` 看不到它（`>>` 静默失败） | 不读参数，只挂地址对（call/返回）就能算时长；注册后**必须 `cat` 复核** |
| **`extract_func.py` 对间接 call 的返回地址失效** | `0x123af85 不在任何 FDE 内` | 该地址是 `call *0x2d8(%rbp)` 的**返回地址**，不是函数起点。手动扫汇编找函数边界 |
| **gdb 栈顺序容易读反** | 以为 ftrace 里的"waker"就是 glcore 线程 | 抓栈才知道谁在 `wait_ms`、谁在 `usleep`。**角色要靠栈确认，不能靠调用关系推断** |

---

## 8. 产物

```
analysis/traces/t9/
├── t9.trace / t9b.trace       角色定位（mb_in + futex）
├── t9_stacks.txt              6 次 gdb 全线程栈（角色纠正依据）
├── t9_waitms.trace            map + wait_ms 配对（263 窗口 / 2293 wait_ms）
└── t9_notify.trace            notify + wcall/wout（唤醒者归属）

analysis/probes/
├── t9_find_waker.sh           ftrace 定位 worker/waker 角色
├── t9_gdb_waker.sh            同一次运行内：ftrace 定角色 → gdb 抓该线程栈
├── t9_waitms.sh               map 与 wait_ms 时长对照（含探针注册校验）
├── t9_align.py                map 窗口与 wait_ms 的重叠/覆盖分析
├── t9_notify.sh               notify 频率 + 调用者归属 + 与窗口的关系
└── t9_whowakes.py             长 wait 的唤醒者归属（本节核心）
```
