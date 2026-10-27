#!/usr/bin/env python3
"""dump_pb.py <pid> [条数] —— 从 /proc/<pid>/mem 读 nvidia 的 pushbuffer 映射并解码
命令头格式（本实验此前反推并验证过）：
  op  = w>>29          0/1 = INC_METHOD
  cnt = (w>>16)&0x1fff 后面跟 cnt 个参数
  sub = (w>>13)&0x7    subchannel
  meth= (w&0x1fff)<<2  方法地址
"""
import sys, struct, re

def u32(b, o): return struct.unpack_from("<I", b, o)[0]

def valid_header(w):
    op = w >> 29
    cnt = (w >> 16) & 0x1fff
    sub = (w >> 13) & 0x7
    if w == 0:
        return False
    return op in (0, 1) and cnt <= 0x40 and sub <= 7

def decode(blob, base_va, limit, out):
    n = len(blob)
    for off in range(0, n - 4, 4):
        if not valid_header(u32(blob, off)):
            continue
        # 试着从这里连解 12 条，全部合法才算起点
        p, ok, cmds, nz = off, True, [], 0
        for _ in range(12):
            if p + 4 > n: ok = False; break
            w = u32(blob, p)
            if not valid_header(w): ok = False; break
            if w: nz += 1
            cnt = (w >> 16) & 0x1fff
            sub = (w >> 13) & 0x7
            meth = (w & 0x1fff) << 2
            cmds.append((sub, meth, cnt, p))
            p += 4 * (cnt + 1)
        if not ok or nz < 8:
            continue
        # 连续解码
        while p + 4 <= n and len(out) < limit:
            w = u32(blob, p)
            if not valid_header(w): break
            cnt = (w >> 16) & 0x1fff
            sub = (w >> 13) & 0x7
            meth = (w & 0x1fff) << 2
            args = [u32(blob, p + 4 * k) for k in range(1, min(cnt, 3) + 1) if p + 4 * k + 4 <= n]
            out.append((base_va + p, sub, meth, cnt, args))
            p += 4 * (cnt + 1)
        if len(out) >= limit:
            return

def main():
    pid = int(sys.argv[1]); limit = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    maps = []
    for l in open(f"/proc/{pid}/maps"):
        if "nvidiactl" not in l and "nvidia" not in l: continue
        p = l.split()
        if "rw" not in p[1]:           # 只看可写的（CPU 写入的 pushbuffer）
            continue
        a, b = p[0].split("-")
        maps.append((int(a, 16), int(b, 16), p[-1]))
    print(f"# pid={pid}  可写 nvidia 映射 {len(maps)} 段")
    out = []
    with open(f"/proc/{pid}/mem", "rb", 0) as mem:
        for (a, b, name) in sorted(maps, key=lambda x: x[0]):
            sz = min(b - a, 1 << 21)
            try:
                mem.seek(a); blob = mem.read(sz)
            except Exception as e:
                print(f"  跳过 {a:#x}: {e}"); continue
            print(f"  段 {a:#x}-{b:#x} ({sz//1024} KiB) 读到 {len(blob)} B")
            decode(blob, a, limit, out)
            if len(out) >= limit: break
    print(f"# 解出 {len(out)} 条命令（前 {limit} 上限）")
    print(f"{'va':>14} {'sub':>3} {'meth':>7} {'cnt':>4}  args")
    for (va, sub, meth, cnt, args) in out:
        print(f"{va:#14x} {sub:>3} {meth:#7x} {cnt:>4}  " + " ".join(f"{x:#x}" for x in args))

main()
