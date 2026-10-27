#!/usr/bin/env python3
"""glcore 静态分析工具（无第三方依赖，靠程序头定位）

子命令：
  funcs   <so>                函数边界统计
  who     <so> <hexaddr>      地址落在哪个函数
  imm     <so> <hexvalue>     搜 4 字节立即数（小端），归属到函数
  callers <so> <hexaddr>      搜所有 call rel32 指向该地址的调用点，归属到函数
  dis     <so> <hexaddr> [n]  反汇编 n 条指令（需要 capstone）
"""
import sys, struct, bisect

PT_LOAD, PT_GNU_EH_FRAME = 1, 0x6474e550

class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        assert d[:4] == b"\x7fELF" and d[4] == 2, "need ELF64"
        phoff = struct.unpack_from("<Q", d, 0x20)[0]
        phes  = struct.unpack_from("<H", d, 0x36)[0]
        phn   = struct.unpack_from("<H", d, 0x38)[0]
        self.loads = []       # (vaddr, off, filesz, flags)
        self.ehframe = None   # (vaddr, off, filesz)
        for i in range(phn):
            o = phoff + i * phes
            t, fl = struct.unpack_from("<II", d, o)
            off, va, _pp, fsz, _msz, _al = struct.unpack_from("<QQQQQQ", d, o + 8)
            if t == PT_LOAD:
                self.loads.append((va, off, fsz, fl))
            elif t == PT_GNU_EH_FRAME:
                self.ehframe = (va, off, fsz)

    def v2o(self, v):
        for va, off, fsz, _fl in self.loads:
            if va <= v < va + fsz:
                return off + (v - va)
        return None

    def o2v(self, o):
        for va, off, fsz, _fl in self.loads:
            if off <= o < off + fsz:
                return va + (o - off)
        return None

    def text(self):
        """可执行段（flags & 1）"""
        for va, off, fsz, fl in self.loads:
            if fl & 1:
                return va, off, fsz
        raise RuntimeError("no executable PT_LOAD")

def _read_enc(d, pos, enc, base):
    fmt, app = enc & 0x0f, enc & 0x70
    if   fmt == 0x03: val, n = struct.unpack_from("<I", d, pos)[0], 4
    elif fmt == 0x0b: val, n = struct.unpack_from("<i", d, pos)[0], 4
    elif fmt == 0x04: val, n = struct.unpack_from("<Q", d, pos)[0], 8
    elif fmt == 0x0c: val, n = struct.unpack_from("<q", d, pos)[0], 8
    else: raise ValueError("enc fmt %#x" % fmt)
    if   app == 0x10: val += pos
    elif app == 0x30: val += base
    return val, n

def func_bounds(elf):
    if not elf.ehframe:
        return []
    addr, off, size = elf.ehframe
    d = elf.d
    p = off
    ptr_enc, cnt_enc, tab_enc = d[p+1], d[p+2], d[p+3]
    p += 4
    _, n1 = _read_enc(d, p, ptr_enc, addr); p += n1
    cnt,  n2 = _read_enc(d, p, cnt_enc, addr); p += n2
    starts = []
    for _ in range(int(cnt)):
        loc, n3 = _read_enc(d, p, tab_enc, addr)
        _f, n4 = _read_enc(d, p + n3, tab_enc, addr)
        starts.append(loc); p += n3 + n4
    return sorted(set(starts))

def containing(starts, a):
    i = bisect.bisect_right(starts, a) - 1
    if i < 0: return None
    s = starts[i]
    e = starts[i+1] if i + 1 < len(starts) else s
    return s, e

def scan_calls(elf, tgt):
    taddr, toff, tsize = elf.text()
    d = elf.d
    mv = d[toff:toff+tsize]
    out = []
    i = mv.find(b"\xE8")
    while i >= 0:
        if i + 5 <= len(mv):
            rel = struct.unpack_from("<i", mv, i+1)[0]
            if (taddr + i + 5 + rel) == tgt:
                out.append(taddr + i)
        i = mv.find(b"\xE8", i + 1)
    return out

def main():
    if len(sys.argv) < 3:
        print(__doc__); return 1
    cmd, elf = sys.argv[1], Elf(sys.argv[2])

    if cmd == "funcs":
        st = func_bounds(elf)
        taddr, toff, tsize = elf.text()
        print("%s" % elf.path.split("/")[-1])
        print("  函数边界 %d 个" % len(st))
        if st:
            print("  范围 0x%x .. 0x%x" % (st[0], st[-1]))
        print("  text vaddr=0x%x off=0x%x size=%.2f MB" % (taddr, toff, tsize/1048576))
        return 0

    starts = func_bounds(elf)

    if cmd == "who":
        a = int(sys.argv[3], 16)
        r = containing(starts, a)
        print("0x%x ∈ func 0x%x..0x%x (len %d, +0x%x)" % (a, r[0], r[1], r[1]-r[0], a-r[0]) if r else "未找到")
        return 0

    if cmd == "imm":
        val = int(sys.argv[3], 16) & 0xffffffff
        pat = struct.pack("<I", val)
        hits, pos = [], 0
        while True:
            i = elf.d.find(pat, pos)
            if i < 0: break
            v = elf.o2v(i)
            if v is not None: hits.append(v)
            pos = i + 1
        print("立即数 0x%08x：%d 处" % (val, len(hits)))
        agg = {}
        for v in hits:
            r = containing(starts, v)
            agg.setdefault(("0x%x" % r[0]) if r else "?", []).append(v)
        for k, vs in sorted(agg.items(), key=lambda kv: -len(kv[1]))[:20]:
            print("  func %-12s %3d 处  %s" % (k, len(vs), " ".join("0x%x" % x for x in vs[:6])))
        return 0

    if cmd == "callers":
        tgt = int(sys.argv[3], 16)
        sites = scan_calls(elf, tgt)
        print("指向 0x%x 的 call：%d 处" % (tgt, len(sites)))
        agg = {}
        for v in sites:
            r = containing(starts, v)
            agg.setdefault(("0x%x" % r[0]) if r else "?", []).append(v)
        for k, vs in sorted(agg.items(), key=lambda kv: -len(kv[1])):
            print("  调用者 func %-12s %d 处  %s" % (k, len(vs), " ".join("0x%x" % x for x in vs[:8])))
        return 0

    if cmd == "dis":
        import capstone
        a = int(sys.argv[3], 16)
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
        off = elf.v2o(a)
        code = elf.d[off:off + n*8]
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        for ins in md.disasm(code, a):
            print("0x%x  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
            n -= 1
            if n <= 0: break
        return 0

    print("未知子命令"); return 1

if __name__ == "__main__":
    sys.exit(main())
