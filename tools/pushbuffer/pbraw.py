#!/usr/bin/env python3
"""pbraw.py <pid> <meth_hex> —— 抓某 method 在 pushbuffer 里的原始 32 字节上下文

用途：拿到命令头的精确编码（不只是解码后的 meth），以便设计"运行时改写"方案。
输出每条命中前后各 4 个 dword（含头部），并标出哪一个是头、哪一个是参数。
"""
import sys, struct, time

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
    pid = int(sys.argv[1]); target = int(sys.argv[2], 16)
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
    shown = 0
    for _ in range(40):
        time.sleep(0.015)
        cur = snap(segs, mem)
        for a, d0 in prev.items():
            d1 = cur.get(a)
            if d1 is None: continue
            n = min(len(d0), len(d1))
            for off in range(0, n, 4096):
                if d0[off:off+4096] == d1[off:off+4096]: continue
                blob = d1[off:off+4096]; base = a + off; p = 0
                while p + 4 <= len(blob) and shown < 6:
                    w = struct.unpack_from("<I", blob, p)[0]
                    if not valid_header(w): p += 4; continue
                    cnt = (w >> 16) & 0x1fff
                    meth = (w & 0x1fff) << 2
                    end = p + 4 * (cnt + 1)
                    if end > len(blob): break
                    if meth == target:
                        lo = max(0, p - 16); hi = min(len(blob), end + 16)
                        print(f"--- 命中 @ va={base+p:#x}  header={w:#010x}  cnt={cnt}  meth={meth:#x} ---")
                        for q in range(lo, hi, 4):
                            v = struct.unpack_from("<I", blob, q)[0]
                            tag = "  <== HEADER" if q == p else ("  (arg)" if p < q < end else "")
                            print(f"   {base+q:#14x} +{q-p:+4d}  {v:#010x}{tag}")
                        shown += 1
                    p = end
        prev = cur
        if shown >= 6: break
    mem.close()
    if shown == 0:
        print("未捕获")

if __name__ == "__main__":
    main()
