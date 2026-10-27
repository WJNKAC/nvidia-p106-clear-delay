#!/usr/bin/env python3
"""desensitize.py —— 把报告/源码里的环境标识替换为占位符

**这个脚本本身就可能是泄露源**：替换规则里会写着真实的 IP、用户名、路径。
所以规则不写死在代码里，而是外置成 `rules.json`，仓库里只提供**假数据示例**
（`rules.example.json`）。

用法
----
    # 1) 复制示例规则，改成你自己的环境
    cp rules.example.json rules.json

    # 2) 执行（rules.json 不要提交到仓库！）
    python3 desensitize.py <输入目录> <输出目录> --rules rules.json

设计要点
--------
1. **规则必须精确匹配**。不要用 `192\\.168\\.` 这种前缀通配，
   也别用 `\\d+\\.\\d+\\.\\d+\\.\\d+` 全量抓 IP —— 版本号会长得一模一样。
   真实踩过的例子：`525.116.3.0`（driverVersion）、`86.06.58.00.21`（VBIOS 版本）
   都能被 IP 正则匹配到，替换后会毁掉报告的技术信息。
2. **先长后短**。`user@ip` 要在 `ip` 之前；`/home/xxx` 要在裸用户名之前。
3. **幂等**。占位符不含原串，重复运行不会叠加替换。
4. **自检**。跑完扫描输出目录，确认原始串一个不剩。
"""
import os, re, sys, json
from collections import Counter

def load_rules(path):
    data = json.load(open(path, encoding="utf-8"))
    rules = []
    for r in data["rules"]:
        rules.append((r["pattern"], r["replace"], r.get("desc", r["pattern"])))
    return rules, data.get("leak_keys", [])

def desensitize_text(s, rules):
    stat = Counter()
    for pat, rep, desc in rules:
        s, n = re.subn(pat, rep, s)
        if n:
            stat[desc] += n
    return s, stat

def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 2
    src, dst = args[0], args[1]
    rules_path = "rules.json"
    if "--rules" in args:
        rules_path = args[args.index("--rules") + 1]
    if not os.path.exists(rules_path):
        print(f"!! 找不到规则文件 {rules_path}")
        print("   请先: cp rules.example.json rules.json 并按你的环境修改")
        return 2

    rules, leak_keys = load_rules(rules_path)
    os.makedirs(dst, exist_ok=True)

    exts = (".md", ".py", ".c", ".h", ".sh", ".json", ".txt")
    total, nfile = Counter(), 0
    for root, _, files in os.walk(src):
        for name in sorted(files):
            if not name.endswith(exts):
                continue
            p = os.path.join(root, name)
            rel = os.path.relpath(p, src)
            outp = os.path.join(dst, rel)
            d = os.path.dirname(outp)
            if d:
                os.makedirs(d, exist_ok=True)
            try:
                raw = open(p, encoding="utf-8").read()
            except Exception:
                continue
            out, st = desensitize_text(raw, rules)
            open(outp, "w", encoding="utf-8").write(out)
            total.update(st)
            nfile += 1

    print(f"处理文件: {nfile}")
    print("替换统计:")
    for desc, n in total.most_common():
        print(f"  {desc:24s} {n}")

    if leak_keys:
        leaks = Counter()
        for root, _, files in os.walk(dst):
            for name in files:
                if not name.endswith(exts):
                    continue
                t = open(os.path.join(root, name), encoding="utf-8", errors="replace").read()
                for k in leak_keys:
                    if k in t:
                        leaks[k] += t.count(k)
        print("\n自检（应全部为 0）:")
        ok = True
        for k in leak_keys:
            v = leaks.get(k, 0)
            if v:
                ok = False
            print(f"  {k:22s} {v:4d}  {'OK' if v == 0 else '!! 残留'}")
        print("\n结果:", "通过" if ok else "有残留，需处理")
        return 0 if ok else 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
