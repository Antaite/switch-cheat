#!/usr/bin/env python3
"""產生 GBK → Unicode 對照表（common/include/gbk_table.hpp），主機端用來修正非 UTF-8 的金手指名稱。
直接用 Python 內建的 gbk 編碼器產生，不需要外部資料。"""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
vals = []
for lead in range(0x81, 0xFF):
    for trail in range(0x40, 0xFF):
        try:
            ch = bytes([lead, trail]).decode("gbk")
            vals.append(ord(ch) if len(ch) == 1 and ord(ch) <= 0xFFFF else 0)
        except UnicodeDecodeError:
            vals.append(0)
out = root / "common/include/gbk_table.hpp"
with open(out, "w", encoding="utf-8") as f:
    f.write("// 自動產生，請勿手動修改（tools/gen_gbk.py）\n#pragma once\n#include <cstdint>\n\nnamespace cheattw {\n\n")
    f.write("// 索引 = (首位元組 - 0x81) * 191 + (次位元組 - 0x40)，0 = 無效\n")
    f.write(f"constexpr uint16_t kGbkLead = 0x81, kGbkTrail = 0x40, kGbkTrailCount = 191;\n")
    f.write(f"constexpr uint16_t kGbk[{len(vals)}] = {{\n")
    for i in range(0, len(vals), 16):
        f.write("    " + ",".join(f"0x{v:X}" for v in vals[i:i + 16]) + ",\n")
    f.write("};\n\n} // namespace cheattw\n")
print(f"{sum(1 for v in vals if v)} 個字 → {out.relative_to(root)}")
