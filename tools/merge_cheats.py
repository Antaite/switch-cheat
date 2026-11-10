#!/usr/bin/env python3
"""
CheatTW 碼庫合併工具

把多個大神的金手指碼庫合併成一個：
  - TitleID + BuildID 都相同才合併（版本不同位址就不同）
  - 用「碼的內容」判斷重複，不看名稱
  - 名稱相同但內容不同 → 兩條都留，名稱加上 [來源]
  - master code 不同的來源分成不同組，各自輸出一個檔
  - 需要 IPS 修補檔的碼庫（例如 bad1dea），修補檔跟著一起輸出
  - 套用中英對照表，輸出給 App 搜尋用的索引

來源種類（sources.json）：
  git   淺層 clone 整個 repo
  zip   下載 zip（例如每日打包的 release），直接在 zip 裡讀，不解壓
  path  本機資料夾或 zip 檔（測試用）
路徑一律自動辨識：只要路徑長得像 .../{TID}/cheats/{BID}.txt 或 .../{TID}/{BID}.txt 就收。

用法：
  python3 merge_cheats.py --sources sources.json --out dist
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

DMNT_MAX_CHEATS = 128          # 就我們所知 dmnt 每個遊戲最多 128 條
DMNT_MAX_OPCODES = 0x100       # 每條碼最多 256 個字組
HEX_WORD = re.compile(r"^[0-9A-Fa-f]{8}$")
CHEAT_PATH = re.compile(r"(?:^|/)(?P<tid>01[0-9A-Fa-f]{14})/(?:cheats/)?(?P<bid>[0-9A-Fa-f]{16})\.txt$")
IPS_PATH = re.compile(r"(?:^|/)exefs_patches/(?P<dir>[^/]+)/(?P<name>[0-9A-Fa-f]{16,64})\.ips$")


# ───────────────────────── 解析 ─────────────────────────

@dataclass
class Cheat:
    name: str
    opcodes: list[int]
    is_master: bool = False
    sources: list[str] = field(default_factory=list)
    zh: str = ""
    category: str = "其他"

    @property
    def digest(self) -> str:
        return hashlib.sha1(" ".join(f"{w:08X}" for w in self.opcodes).encode()).hexdigest()


def parse_cheat_text(text: str) -> tuple[Cheat | None, list[Cheat], list[str]]:
    """回傳 (master, 一般碼列表, 警告)"""
    master: Cheat | None = None
    cheats: list[Cheat] = []
    warnings: list[str] = []
    cur: Cheat | None = None

    for no, raw in enumerate(text.lstrip("﻿").splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line[0] in "[{":
            close = "]" if line[0] == "[" else "}"
            end = line.rfind(close)
            name = line[1:end if end > 0 else len(line)].strip()
            c = Cheat(name=name, opcodes=[], is_master=line[0] == "{")
            if c.is_master:
                if master is not None:
                    warnings.append(f"第 {no} 行：重複的 master code，已忽略")
                    cur = None
                    continue
                master = c
            else:
                cheats.append(c)
            cur = c
            continue
        for w in line.split():
            if not HEX_WORD.match(w):
                warnings.append(f"第 {no} 行：無法辨識「{w[:20]}」")
                break
            if cur is None:
                warnings.append(f"第 {no} 行：碼前面沒有名稱")
                break
            cur.opcodes.append(int(w, 16))

    cheats = [c for c in cheats if c.opcodes]          # 拿掉只有標題的分隔線
    for c in cheats:
        if len(c.opcodes) > DMNT_MAX_OPCODES:
            warnings.append(f"「{c.name}」超過 {DMNT_MAX_OPCODES} 個字組，dmnt 無法載入")
    cheats = [c for c in cheats if len(c.opcodes) <= DMNT_MAX_OPCODES]
    if master is not None and not master.opcodes:
        master = None
    return master, cheats, warnings


def to_cheat_text(master: Cheat | None, cheats: list[Cheat]) -> str:
    out = []
    for c in ([master] if master else []) + cheats:
        out.append(("{%s}" if c.is_master else "[%s]") % c.name)
        words = [f"{w:08X}" for w in c.opcodes]
        for i in range(0, len(words), 3):
            out.append(" ".join(words[i:i + 3]))
        out.append("")
    return "\n".join(out)


def decode(data: bytes) -> str:
    """金手指檔的編碼很雜：UTF-8 為主，舊的簡體碼常是 GBK，繁體偶爾是 Big5。
    UTF-16 只在有 BOM 時才用（沒 BOM 的話任何偶數長度的資料都能「解」成亂碼）。"""
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        try:
            return data.decode("utf-16")
        except UnicodeDecodeError:
            pass
    for enc in ("utf-8-sig", "gb18030", "big5", "shift_jis"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            pass
    return data.decode("utf-8", errors="replace")


class S2T:
    """簡轉繁：有 opencc 套件就用片語級的 s2twp（台灣用語），沒有就用 data/s2t_chars.tsv 單字表"""

    def __init__(self, table: Path):
        self.cc = None
        try:
            import opencc  # type: ignore
            self.cc = opencc.OpenCC("s2twp")
        except Exception:
            self.cc = None
        self.map = {}
        if self.cc is None and table.exists():
            for line in table.read_text(encoding="utf-8").splitlines():
                if "\t" in line:
                    a, b = line.split("\t", 1)
                    self.map[a] = b

    @property
    def mode(self) -> str:
        return "opencc s2twp" if self.cc else f"單字表（{len(self.map)} 字）"

    def __call__(self, s: str) -> str:
        if not any("\u2e80" <= ch for ch in s):
            return s
        if self.cc:
            return self.cc.convert(s)
        return "".join(self.map.get(ch, ch) for ch in s)


# ───────────────────────── 翻譯 ─────────────────────────

class Translator:
    def __init__(self, tsv: Path | None, s2t: "S2T | None" = None):
        self.s2t = s2t
        self.entries: list[tuple[re.Pattern, str, str]] = []
        if tsv and tsv.exists():
            rows = []
            for line in tsv.read_text(encoding="utf-8").splitlines():
                if not line.strip() or line.startswith("#"):
                    continue
                cols = (line.split("\t") + ["", ""])[:3]
                en, zh, cat = (c.strip() for c in cols)
                if en and zh:
                    rows.append((en, zh, cat))
            rows.sort(key=lambda r: len(r[0]), reverse=True)   # 長詞優先
            self.entries = [(re.compile(r"(?<![A-Za-z0-9])" + re.escape(en) + r"(?![A-Za-z0-9])", re.I), zh, cat)
                            for en, zh, cat in rows]

    def translate(self, name: str) -> tuple[str, str]:
        # 先換成佔位符號，避免翻好的字被短詞再比對一次（例：60 FPS → 60FPS 解鎖 → 又被 60FPS 吃掉）
        result, category, slots = name, "", []
        for pat, zh, cat in self.entries:
            result, n = pat.subn(f"\x00{len(slots)}\x00", result)
            if n:
                slots.append(zh)
                if not category and cat:
                    category = cat
        result = re.sub(r"\x00(\d+)\x00", lambda m: slots[int(m.group(1))], result)
        result = re.sub(r"(?<=[^\x00-\x7F]) (?=[^\x00-\x7F])", "", result)   # 收掉中文之間的空白
        if self.s2t:
            result = self.s2t(result)
        return result, category or "其他"


# ───────────────────────── 讀取來源 ─────────────────────────

class SourceReader:
    """把 git／zip／本機資料夾統一成：列出金手指檔、列出 IPS 修補檔"""

    def __init__(self, src: dict, cache: Path, base: Path):
        self.src, self.cache, self.base = src, cache, base
        self.root: Path | None = None
        self.zip: zipfile.ZipFile | None = None
        self.error = ""

    def open(self) -> bool:
        s = self.src
        try:
            if s.get("path"):
                p = Path(s["path"]).expanduser()
                p = p if p.is_absolute() else self.base / p
                if p.suffix.lower() == ".zip":
                    self.zip = zipfile.ZipFile(p)
                elif p.is_dir():
                    self.root = p
                else:
                    self.error = f"找不到 {p}"
            elif s.get("git"):
                dest = self.cache / s["id"]
                if dest.exists():
                    subprocess.run(["git", "-C", str(dest), "pull", "--ff-only", "-q"], check=False)
                else:
                    r = subprocess.run(["git", "clone", "--depth", "1", "-q", s["git"], str(dest)])
                    if r.returncode != 0:
                        self.error = "git clone 失敗"
                        return False
                self.root = dest
            elif s.get("zip"):
                dest = self.cache / f"{s['id']}.zip"
                req = urllib.request.Request(s["zip"], headers={"User-Agent": "CheatTW-merge"})
                with urllib.request.urlopen(req, timeout=300) as r, open(dest, "wb") as f:
                    shutil.copyfileobj(r, f)
                self.zip = zipfile.ZipFile(dest)
            else:
                self.error = "沒有設定 git／zip／path"
        except Exception as e:      # 任何一個來源壞掉都不能讓整個合併失敗
            self.error = f"{type(e).__name__}: {e}"
        return self.root is not None or self.zip is not None

    def _names(self):
        if self.zip:
            for n in self.zip.namelist():
                if not n.endswith("/"):
                    yield n
        else:
            for f in self.root.rglob("*"):
                if f.is_file() and ".git" not in f.parts:
                    yield f.relative_to(self.root).as_posix()

    def _read(self, name: str) -> bytes:
        return self.zip.read(name) if self.zip else (self.root / name).read_bytes()

    def cheat_files(self):
        for n in self._names():
            m = CHEAT_PATH.search(n)
            if m:
                yield m.group("tid").upper(), m.group("bid").upper(), lambda n=n: decode(self._read(n))

    def ips_files(self):
        for n in self._names():
            m = IPS_PATH.search(n)
            if m:
                yield m.group("dir"), m.group("name").upper(), lambda n=n: self._read(n)


# ───────────────────────── 合併 ─────────────────────────

@dataclass
class Group:
    master: Cheat | None
    cheats: list[Cheat] = field(default_factory=list)
    sources: list[str] = field(default_factory=list)


def merge_build(entries: list[tuple[str, Cheat | None, list[Cheat]]]) -> list[Group]:
    """entries：依來源優先順序排好的 (來源, master, 碼)。回傳的第一組是主檔。"""
    groups: dict[str | None, Group] = {}
    no_master: list[tuple[str, list[Cheat]]] = []

    for sid, master, cheats in entries:
        if master is None:
            no_master.append((sid, cheats))
            continue
        g = groups.setdefault(master.digest, Group(master=master))
        if sid not in master.sources:
            master.sources.append(sid)
        add_cheats(g, sid, cheats)

    # 沒有 master 的碼不依賴任何主碼，放進每一組
    if not groups:
        groups[None] = Group(master=None)
    for g in groups.values():
        for sid, cheats in no_master:
            add_cheats(g, sid, cheats)

    for g in groups.values():
        resolve_name_conflicts(g)
    # 來源越多的組排越前面；一樣多就維持原本（來源優先順序）
    return sorted(groups.values(), key=lambda g: -len(g.sources))


def add_cheats(g: Group, sid: str, cheats: list[Cheat]):
    if sid not in g.sources:
        g.sources.append(sid)
    by_digest = {c.digest: c for c in g.cheats}
    for c in cheats:
        hit = by_digest.get(c.digest)
        if hit:
            if sid not in hit.sources:
                hit.sources.append(sid)
            continue
        nc = Cheat(name=c.name, opcodes=list(c.opcodes), sources=[sid])
        g.cheats.append(nc)
        by_digest[nc.digest] = nc


def resolve_name_conflicts(g: Group):
    seen: dict[str, int] = {}
    for c in g.cheats:
        seen[c.name.lower()] = seen.get(c.name.lower(), 0) + 1
    for c in g.cheats:
        if seen[c.name.lower()] > 1:
            c.name = f"{c.name} [{c.sources[0]}]"


# ───────────────────────── 遊戲名稱 ─────────────────────────

def load_titles(manual: Path | None, titledb_dir: Path | None) -> tuple[dict[str, str], dict[str, str]]:
    """回傳 (中文名, 英文名)。優先順序：手動 titles_zh.tsv > titledb 台灣 > 香港 > 美國英文"""
    zh: dict[str, str] = {}
    en: dict[str, str] = {}

    def from_titledb(fname: str, target: dict):
        if not titledb_dir:
            return
        p = titledb_dir / fname
        if not p.exists():
            return
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            return
        for v in (data.values() if isinstance(data, dict) else data):
            if isinstance(v, dict) and v.get("id") and v.get("name"):
                tid = str(v["id"]).upper()
                target.setdefault(tid, str(v["name"]).strip())

    from_titledb("TW.zh.json", zh)
    from_titledb("HK.zh.json", zh)
    from_titledb("US.en.json", en)
    if manual and manual.exists():
        for line in manual.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#") and "\t" in line:
                tid, name = line.split("\t", 1)
                zh[tid.strip().upper()] = name.strip()       # 手動的最優先
    return zh, en


# ───────────────────────── 主流程 ─────────────────────────

def main(argv=None) -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="CheatTW 碼庫合併工具")
    ap.add_argument("--sources", type=Path, default=here / "sources.json")
    ap.add_argument("--dict", type=Path, default=here.parent / "data/translations/zh_TW.tsv")
    ap.add_argument("--titles", type=Path, default=here.parent / "data/titles_zh.tsv")
    ap.add_argument("--titledb", type=Path, default=None, help="放 TW.zh.json / HK.zh.json / US.en.json 的資料夾")
    ap.add_argument("--cache", type=Path, default=here / ".cache")
    ap.add_argument("--out", type=Path, default=here / "dist")
    ap.add_argument("--only", nargs="*", help="只跑指定 id 的來源（測試用）")
    args = ap.parse_args(argv)

    cfg = json.loads(args.sources.read_text(encoding="utf-8"))
    sources = [s for s in cfg["sources"] if s.get("enabled", True) and (not args.only or s["id"] in args.only)]
    s2t = S2T(here.parent / "data/s2t_chars.tsv")
    translator = Translator(args.dict, s2t)
    print(f"[簡轉繁] {s2t.mode}")
    zh_names, en_names = load_titles(args.titles, args.titledb)
    args.cache.mkdir(parents=True, exist_ok=True)
    if args.out.exists():
        # 只清掉自己產生過的輸出資料夾，避免 --out 打錯把別的東西刪掉
        if (args.out / "index.json").exists() or not any(args.out.iterdir()):
            shutil.rmtree(args.out)
        else:
            print(f"[錯誤] {args.out} 不是空資料夾，也不是之前的輸出，請換一個 --out", file=sys.stderr)
            return 1
    db = args.out / "db"

    # 1. 收集：(tid, bid) → [(來源, master, 碼)]；sources.json 的順序 = 優先順序
    builds: dict[tuple[str, str], list] = {}
    patches: dict[str, dict[str, tuple[str, callable]]] = {}    # 來源 → BID → (資料夾, 讀檔)
    report = {"sources": {}, "warnings": []}
    for src in sources:
        reader = SourceReader(src, args.cache, args.sources.parent)
        if not reader.open():
            report["sources"][src["id"]] = f"無法取得：{reader.error}"
            print(f"[略過] {src['id']}：{reader.error}", file=sys.stderr)
            continue
        n = 0
        for tid, bid, read in reader.cheat_files():
            master, cheats, warns = parse_cheat_text(read())
            report["warnings"] += [f"{src['id']} {tid}/{bid}：{w}" for w in warns[:5]]
            if master or cheats:
                builds.setdefault((tid, bid), []).append((src["id"], master, cheats))
                n += 1
        np_ = 0
        if src.get("patches"):
            for d, name, read in reader.ips_files():
                patches.setdefault(src["id"], {})[name[:16]] = (name, read)
                np_ += 1
        report["sources"][src["id"]] = f"{n} 個檔" + (f"、{np_} 個修補檔" if np_ else "")
        print(f"[讀取] {src['id']}：{report['sources'][src['id']]}")

    # 2. 合併並輸出
    index: dict = {
        "version": 2,
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "sources": [{k: s.get(k, "") for k in ("id", "name", "url", "license")} for s in sources],
        "titles": {},
    }
    stats = {"builds": 0, "cheats": 0, "duplicates": 0}
    per_title: dict[str, dict] = {}

    for (tid, bid), entries in sorted(builds.items()):
        groups = merge_build(entries)
        tdir = db / tid
        tdir.mkdir(parents=True, exist_ok=True)
        detail = per_title.setdefault(tid, {})
        meta_lines, files = [], []

        for gi, g in enumerate(groups):
            for c in g.cheats:
                c.zh, c.category = translator.translate(c.name)
            fname = f"{bid}.txt" if gi == 0 else f"{bid}.alt{gi}.txt"
            (tdir / fname).write_text(to_cheat_text(g.master, g.cheats), encoding="utf-8")

            # 這組有用到需要修補檔的來源 → 修補檔一起輸出
            patch_files = []
            for sid in g.sources:
                hit = patches.get(sid, {}).get(bid)
                if hit:
                    pname, read = hit
                    pdir = tdir / "patches" / sid
                    pdir.mkdir(parents=True, exist_ok=True)
                    (pdir / f"{pname}.ips").write_bytes(read())
                    patch_files.append(f"patches/{sid}/{pname}.ips")

            total = len(g.cheats) + (1 if g.master else 0)
            # overlay 讀這個簡單的 TSV，不用解析 JSON：檔名、條數、是否要修補檔、來源
            meta_lines.append("\t".join([fname, str(len(g.cheats)), "1" if patch_files else "0",
                                         ",".join(g.sources)]))
            files.append({
                "file": fname, "sources": g.sources, "master": bool(g.master),
                "count": len(g.cheats), "over_limit": total > DMNT_MAX_CHEATS, "patches": patch_files,
                "cheats": [{"name": c.name, "zh": c.zh, "category": c.category, "sources": c.sources}
                           for c in g.cheats],
            })
            stats["cheats"] += len(g.cheats)
            stats["duplicates"] += sum(len(c.sources) - 1 for c in g.cheats)

        (tdir / f"{bid}.meta.tsv").write_text("\n".join(meta_lines) + "\n", encoding="utf-8")
        title_name = zh_names.get(tid) or en_names.get(tid)
        if title_name:
            (tdir / "title.txt").write_text(s2t(title_name) + "\n", encoding="utf-8")   # overlay 顯示遊戲名稱用
        detail[bid] = files
        t = index["titles"].setdefault(tid, {"name": zh_names.get(tid, ""), "name_en": en_names.get(tid, ""),
                                            "builds": {}})
        t["builds"][bid] = {"groups": len(groups), "count": files[0]["count"],
                            "needs_patch": bool(files[0]["patches"])}
        stats["builds"] += 1

    # 每款遊戲一份詳細索引，App 只下載正在玩的那款，總索引保持很小
    for tid, detail in per_title.items():
        (db / tid / "index.json").write_text(json.dumps(detail, ensure_ascii=False), encoding="utf-8")

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "index.json").write_text(json.dumps(index, ensure_ascii=False, separators=(",", ":")),
                                         encoding="utf-8")
    report["stats"] = stats
    (args.out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"[完成] {len(index['titles'])} 款遊戲、{stats['builds']} 個版本、"
          f"{stats['cheats']} 條碼（合併掉 {stats['duplicates']} 條重複）→ {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
