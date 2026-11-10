#!/usr/bin/env python3
"""
從 OpenCC 字典產生「簡體字 → 台灣繁體字」單字對照表：
  - common/include/s2t_table.hpp   主機端用（排序好的陣列，二分搜尋）
  - data/s2t_chars.tsv             合併工具沒有 opencc 套件時的備援

用法：python3 gen_s2t.py <OpenCC 原始碼資料夾>
資料來源：OpenCC（Apache-2.0）data/dictionary/STCharacters.txt + TWVariants.txt
"""
import sys
from pathlib import Path

opencc = Path(sys.argv[1]) / "data" / "dictionary"
root = Path(__file__).resolve().parent.parent


def load(name):
    m = {}
    for line in (opencc / name).read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or "\t" not in line:
            continue
        src, dst = line.split("\t", 1)
        cand = dst.split()[0]
        if len(src) == 1 and len(cand) == 1:      # 只要單字對單字
            m[src] = cand
    return m


st = load("STCharacters.txt")
tw = load("TWVariants.txt")                       # 轉成台灣慣用字形
table = {}
for s, t in st.items():
    t = tw.get(t, t)
    if s != t:
        table[s] = t
for s, t in tw.items():
    table.setdefault(s, t)

pairs = sorted((ord(s), ord(t)) for s, t in table.items())

hpp = root / "common/include/s2t_table.hpp"
with open(hpp, "w", encoding="utf-8") as f:
    f.write("// 自動產生，請勿手動修改（tools/gen_s2t.py）\n")
    f.write("// 資料來源：OpenCC STCharacters + TWVariants（Apache-2.0）\n")
    f.write("#pragma once\n#include <cstdint>\n\nnamespace cheattw {\n\n")
    f.write("struct S2TPair { uint32_t s; uint32_t t; };\n\n")
    f.write(f"constexpr S2TPair kS2T[{len(pairs)}] = {{\n")
    for i in range(0, len(pairs), 6):
        f.write("    " + " ".join(f"{{0x{s:X},0x{t:X}}}," for s, t in pairs[i:i + 6]) + "\n")
    f.write("};\n\n} // namespace cheattw\n")

tsv = root / "data/s2t_chars.tsv"
tsv.write_text("".join(f"{chr(s)}\t{chr(t)}\n" for s, t in pairs), encoding="utf-8")
print(f"{len(pairs)} 個字 → {hpp.relative_to(root)}、{tsv.relative_to(root)}")
