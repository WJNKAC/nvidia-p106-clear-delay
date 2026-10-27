# vBIOS 功耗表解析报告（WIP）

- **日期**: 2026-10-24
- **状态**: **受阻**——镜像提取与结构解析成功，但 perf/功耗表区在刷写包容器中非明文存储，静态路线暂断；下一步转上机只读 dump（见报告 03）
- **目标**: 区分 H1/H2 假说（见报告 01 第 5 节）——读 P106-100 vBIOS 的 Rated TDP / Max Power Limit / Default Power Limit 字段

---

## 1. 材料

| 项 | 值 |
|---|---|
| 来源 | 技嘉官网 GV-NP106D5-6G (rev 1.1/1.2/1.3) 支持页面 |
| 文件 | `vbios/GV-NP106D56.FX1.ROM.zip`（4.98MB） |
| BIOS 版本 | **86.06.58.00.21**（NVIDIA Source BIOS Version，镜像内字符串证实） |
| 板卡标识 | GV-NP106D5-6G/FX1/061B |
| 频率 | Base/Boost 1506/1708 MHz，显存 8008 MHz |
| 容器 | Windows 刷写工具（x64/x86 exe + MFC DLL），ROM 内嵌 |

## 2. 容器分析结论

exe（x64，13.98MB）内 ROM 相关区域：

| 区域 | 文件偏移 | 大小 | 内容 |
|---|---|---|---|
| 描述符+image1 | 0x4a2184 | 0xF200 (61952B) | `55 AA 79` 头、入口代码、"VIDEO"/"IBM VGA Compatible"、"PMID"、板卡名、版本串、版权、"GP106 Board"、"Chip Rev"、**BIT@+0x1e2**、**P2 指针表@+0x2ee**、PCIR(10DE/1C07, imglen=0x79)@+0x1a0、NPDE@+0x1be |
| image2 | 0x4b1384 | 0x10800 (67584B) | `55 AA 84` 头、PCIR(10DE/1C07, imglen=0x84)@+0x1c、NPDE@+0x40，**无 BIT** |

其他 "PCIR"/"BIT" 命中（0x192d7b、0x3d8ff2、0x3ef13a 等）均为压缩/随机数据中的假阳性（厂商/设备 ID 校验不通过）。

**镜像内存在 XOR 0xFF 反相存储的字符串区**（如 "Engineering Release - Not For Production Use"），属 vBIOS 自带混淆，非容器损坏。

## 3. 已解析成功的部分

BIT 条目表（17 条，(id, ver, len16, off16) 格式）完整解析，关键条目：

```
'B' v2 len 0x25 off 0x026e      'C' v2 len 0x1c off 0x0293
'D' v1 len 0x04 off 0x02af      'I' v1 len 0x22 off 0x02b3
'M' v2 len 0x19 off 0x02d5      'N' v0 len 0x00 off 0x0000
'P' v2 len 0x9c off 0x02ee  ← P2 功耗/性能指针表
'S' v2 len 0x18 off 0x038a      'T' v1 ... 'U' 'V' 'x' 'd' 'p' 'u' 'i' ...
```

**P2 指针表（0x2ee，39 个 u32）完整读出且全部合理**：

```
PERFORMANCE      -> 0x01d155      POWER SENSE       -> 0x01f97b
MEM TIMINGS MAP  -> 0x01d4ca      POWER BUDGET      -> 0x01fd69   ★
MEMORY TIMINGS   -> 0x01d848      POWER TOPOLOGY    -> 0x01fa16
POWER BASE CLOCK -> 0x01d243      POWER LEAKAGE     -> 0x020307
PERF TEST SPEC   -> 0x01d4bc      THERMAL DEVICES   -> 0x020495
VOLTAGE RAIL     -> 0x004e63      (VOLTAGE/THERMAL/BOOST/CSTEP 等为空指针 0)
```

envytools `nvbios`（已从源码编译，`vbios/tools/envytools-src/build/nvbios/nvbios`）对 image1 的解析与上述手动解析一致，其 POWER BUDGET 报错offset（0x1fd69 等）与指针表完全对应——**指针解析链路正确**。

## 4. 受阻点

P2 指针值（0x1d155–0x2056b）**超出 image1 尺寸 0xF200**，且：

- 全 exe 扫描 POWER BUDGET 表签名（version 0x10/0x20/0x30 + hlen/rlen/num + mW 量级条目）**零命中**
- 以 image1/image2/PCIR-0x18 等为基准解析指针均得到非表数据（乱码或零区）
- image2 无 BIT，其 0x1d000+ 区域全零

**结论**：该刷写包容器对 perf/功耗表区采用了非明文存储（压缩/变换/分块），静态提取需要先逆出容器格式，性价比骤降。

## 5. 路径选择

| 方案 | 说明 | 状态 |
|---|---|---|
| A. 上机只读 dump vBIOS | <testhost> 实卡，read-only，权威数据 | **推荐**（命令见报告 03） |
| B. 其他 raw ROM 源 | TechPowerUp 有 bot 校验（直接 .rom 下载返回 HTML）；chinafix/CSDN/tieba 均需登录 | 暂不可用 |
| C. 继续逆容器格式 | 需先定位压缩/变换算法，时间不可控 | 暂缓 |

## 6. 复现工具

- `vbios/tools/parse_vbios.py`：镜像提取 + BIT/P2 指针解析（本文第 3 节数据的生成脚本）
- `vbios/tools/envytools-src/`：envytools 源码 + 已编译的 nvbios
- `vbios/P106-100-Gigabyte-FX1.rom`：image1（61952B，含 BIT/P 表）
- `vbios/P106-100-Gigabyte-FX1-image2.rom`：image2（67584B）
- `analysis/`：驱动侧符号表与 diff 产物
