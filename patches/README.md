# patches —— 驱动补丁集

按驱动版本组织的 `libnvidia-glcore.so` 字节补丁。

---

## 目录约定

```
patches/
├── README.md            ← 本文件
├── patch_glcore.py      通用工具（check / apply / revert / verify）
└── <驱动版本>/
    ├── patch.json       补丁定义：偏移 + 期望原值 + 新值 + 前后 md5 + 原理 + 限制
    ├── README.md        该版本的详细说明（原理 / 实测量据 / 风险 / 重定位方法）
    ├── apply.sh         一键应用（自动备份、检查 root）
    └── revert.sh        一键回滚
```

### 本仓库不包含 `before/` 与 `after/` 二进制

原始 `.so` 有 35–44 MB，且属于 NVIDIA 专有版权软件，不适合随仓库分发。

**需要时自己生成：**

```bash
# 备份原版
sudo cp /usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.<版本> /tmp/before.so

# 生成补丁版
sudo cp /tmp/before.so /tmp/after.so
sudo python3 patch_glcore.py apply /tmp/after.so --spec <版本>/patch.json

# 校验：两个文件的 md5 应分别等于 patch.json 里的 before_md5 / after_md5
md5sum /tmp/before.so /tmp/after.so
```

`patch.json` 里记录了这两个 md5，可以据此确认"你手里的文件和我验证过的是同一份"。

---

## 现有补丁

| 版本 | 补丁处数 | 策略位偏移 | 原版 `map_p50` | 补丁后 | 状态 |
|---|---|---|---|---|---|
| [525.116.03](525.116.03/README.md) | **2** | `+0xc18` | 52.276 ms | 3.448 ms | 已验证 |
| [580.178.04](580.178.04/README.md) | **1** | `+0x20b` | 52.195 ms | 3.434 ms | 已验证 |

**为什么 580 只要 1 处、525 要 2 处**：525 有两个发射点都会在正常绘制路径上被调用；
580 的第二个发射点（`0xde855e`）结构改成了"无条件循环发射"，
但**实测在测试负载下根本不被调用**（入口硬件断点 5 秒 0 命中），所以不需要处理。

> ⚠ 但 580 的第二处**不是死代码** —— 它是另一条路径（可能是别的绘制模式），
> 其他应用可能走到。如果遇到"某些程序还是慢"，先查 `0xde81e0` 有没有被调用。

---

## 通用约束（每个补丁都必须满足）

### 1. 必须做三层验证

| 层 | 做什么 | 为什么不能省 |
|---|---|---|
| ① 磁盘 | 读回文件字节 | 基础确认 |
| ② **进程内存** | 起一个 GL 程序，读 `/proc/<pid>/mem` | **磁盘改了 ≠ 进程用的是新文件**（可能有多份同名库、软链、`ld.so.cache` 指向别处） |
| ③ 行为 | 性能数字 **+ 目标命令是否真的消失** | 只快不"对"就是改错了地方 |

第 ③ 层最好直接观察**被跳过的东西是否不在了**（目标 GPU 命令的样本数归零），
这比"耗时下降"更能证明补丁走到了预期路径。

### 2. 必须验副作用

每次都要看**像素校验值**是否与原版一致。**一改就快但画面错了，
说明那段逻辑是正确性必需的，不能跳。**

### 3. 回滚要有三条路

1. `revert.sh`（从 `.orig` 备份）
2. 手动从自己的备份复制
3. **重装同版本驱动**（安装器会写回原始文件）—— 最终保险

### 4. 补丁会在这些情况下失效

- **驱动升级**：偏移随版本变化
- **重装同版本驱动**：文件被覆盖
- **包校验**（`dpkg -V` / rpm）：会报告该文件被修改，属预期

### 5. 这不是"修好了驱动"

在别人的二进制里跳过一段逻辑，属于**逆向研究成果的固化**。
生产环境应优先：换用无此问题的版本，或在应用层合批。

---

## 新增一个版本补丁的流程

```
1. 运行时验证（LD_PRELOAD 代码补丁），确认性能提升且像素校验不变
2. 记录该版本原文件的 md5
3. 写 patch.json（偏移 / 原值 / 新值 / 两个 md5 / 原理 / 限制）
4. ★ 自证：从原版重新打补丁，确认能逐字节复现
5. 三层验证（磁盘 / 内存 / 行为）
6. 写该版本的 README.md
```

**第 4 步不能省**：能复现才说明 `patch.json` 是完整的（没漏掉任何一处改动）。

```bash
# 自证示例
cp before.so /tmp/t.so
python3 patch_glcore.py apply /tmp/t.so --spec <版本>/patch.json
cmp /tmp/t.so after.so && echo "逐字节一致"
```

---

## 工具用法

```bash
# 查状态（不改动）
sudo python3 patch_glcore.py check  /usr/lib/.../libnvidia-glcore.so.<版本> --spec <版本>/patch.json

# 应用（自动备份为 .orig）
sudo python3 patch_glcore.py apply  /usr/lib/.../libnvidia-glcore.so.<版本> --spec <版本>/patch.json

# 回滚
sudo python3 patch_glcore.py revert /usr/lib/.../libnvidia-glcore.so.<版本>

# 校验 md5 是否与记录一致
python3 patch_glcore.py verify /usr/lib/.../libnvidia-glcore.so.<版本> --spec <版本>/patch.json
```

**`check` 能区分三种状态**：未打补丁 / 已打补丁 / **未知值（位置可能不对）**。
出现"未知"说明偏移漂移了，**不要继续 apply**。
