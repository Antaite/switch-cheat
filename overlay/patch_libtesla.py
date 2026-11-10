#!/usr/bin/env python3
"""
編譯前修改 libtesla 的字型載入：

原版只有在主機「系統語言」設成中文時才載入中文字型，其他語言（日文、英文）
只有日文字型，很多中文字會變成方塊。這裡改成：系統語言不是中文／韓文時，
一律額外載入繁體中文系統字型當備援。

用法：python3 patch_libtesla.py libs/libtesla/include/tesla.hpp
"""
import sys
from pathlib import Path

path = Path(sys.argv[1] if len(sys.argv) > 1 else "libs/libtesla/include/tesla.hpp")
src = path.read_text(encoding="utf-8")

old = """                    default:
                        this->m_hasLocalFont = false;
                        break;"""
new = """                    default:
                        // CheatTW：非中文系統也載入繁中字型，避免中文變方塊
                        TSL_R_TRY(plGetSharedFontByType(&localFontData, PlSharedFontType_ChineseTraditional));
                        break;"""

if "CheatTW：非中文系統也載入繁中字型" in src:
    print("libtesla 已經修改過")
elif old in src:
    path.write_text(src.replace(old, new, 1), encoding="utf-8")
    print("libtesla 字型修改完成")
else:
    sys.exit("找不到 libtesla 的字型程式碼，libtesla 可能改版了，請檢查 initFonts()")
