#!/usr/bin/env python3
"""pbargs.py <pid> <meth_hex> [...] —— 抓指定 method 的参数样本

只统计"变化中的 4KB 块"里的命令，匹配目标 method 后打印前若干条完整参数。
用于判断某个 method 的语义（比如是不是 semaphore 写入：参数通常是 [地址低32, 地址高32, 值]）。
"""
import sys, struct, time

def u32(b, o): return struct.unpack_from("<I", b, o)[0]

def valid_header(w):
    if w == 0: return False
    return (w >> 29) in (0, 1) and 1 <= ((w >> 16) & 0x1fff) <= 64 and ((w >> 13) & 7) <= 7

def snap(segs, mem):
    out = {}
    for (a, b) in segs:
        try:
            mem.seek(a); out[a] = mem.read(min(b-a, 1 << 20))
        except Exception:
            pass
    return out

def main():
    pid = int(sys.argv[1])
    targets = set(int(x, 16) for x in sys.argv[2:]) or {0x39c0}
    segs = []
    for l in open(f"/proc/{pid}/maps"):
        if "nvidia" not in l: continue
        p = l.split()
        if "rw" not in p[1]: continue
        a, b = p[0].split("-")
        segs.append((int(a, 16), int(b, 16)))
    segs.sort()
    mem = open(f"/proc/{pid}/mem", "rb", 0)
    prev = snap(segs, mem)
    hits = {t: [] for t in targets}
    for _ in range(40):
        time.sleep(0.015)
        cur = snap(segs, mem)
        for a, d0 in prev.items():
            d1 = cur.get(a)
            if d1 is None: continue
            n = min(len(d0), len(d1))
            for off in range(0, n, 4096):
                if d0[off:off+4096] == d1[off:off+4096]: continue
                blob = d1[off:off+4096]; p = 0
                while p + 4 <= len(blob):
                    w = u32(blob, p)
                    if not valid_header(w): p += 4; continue
                    cnt = (w >> 16) & 0x1fff
                    meth = (w & 0x1fff) << 2
                    end = p + 4 * (cnt + 1)
                    if end > len(blob): break
                    if meth in targets and len(hits[meth]) < 40:
                        args = [u32(blob, p + 4*k) for k in range(1, cnt+1)]
                        hits[meth].append(args)
                    p = end
        prev = cur
    mem.close()
    for t, rows in hits.items():
        print(f"=== method 0x{t:x}  样本 {len(rows)} 条 ===")
        for r in rows[:8]:
            print("   cnt=%d  " % len(r) + " ".join(f"{x:#010x}" for x in r[:8])
                  + ("  ..." if len(r) > 8 else ""))
        if not rows:
            print("   （未捕获）")

if __name__ == "__main__":
    main()
