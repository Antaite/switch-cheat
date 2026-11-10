"""合併腳本測試：在 tools/ 底下執行 python3 -m unittest tests/test_merge.py"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import merge_cheats as mc  # noqa: E402

T, B = "0100AAAA00000000", "1A2B3C4D5E6F7081"   # 兩個來源、master 相同
C, D = "0100BBBB00000000", "99887766554433AA"   # 兩個來源、master 不同
E, F = "0100CCCC00000000", "ABCDEF0123456789"   # zip 來源 + 需要 IPS 的來源


class MergeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.dist = cls.tmp / "dist"
        rc = mc.main(["--sources", str(HERE / "sources_test.json"), "--out", str(cls.dist),
                      "--cache", str(cls.tmp / "cache"), "--titledb", str(HERE / "fixtures/titledb"),
                      "--titles", str(HERE / "fixtures/titles_zh.tsv")])
        assert rc == 0
        cls.index = json.loads((cls.dist / "index.json").read_text(encoding="utf-8"))
        cls.report = json.loads((cls.dist / "report.json").read_text(encoding="utf-8"))

    def detail(self, tid, bid, group=0):
        d = json.loads((self.dist / "db" / tid / "index.json").read_text(encoding="utf-8"))
        return d[bid][group]

    def zh(self, tid, bid, group=0):
        return [c["zh"] for c in self.detail(tid, bid, group)["cheats"]]

    def test_same_code_merged(self):
        hp = [c for c in self.detail(T, B)["cheats"] if c["zh"] == "無限血量"]
        self.assertEqual(len(hp), 1)
        self.assertEqual(hp[0]["sources"], ["範例A", "範例B"])

    def test_same_name_different_code_kept(self):
        names = [c["name"] for c in self.detail(T, B)["cheats"]]
        self.assertIn("Max Money [範例A]", names)
        self.assertIn("Max Money [範例B]", names)

    def test_translation(self):
        zh = self.zh(T, B)
        self.assertNotIn("--- Misc ---", zh)
        self.assertIn("月球跳 [按住 R]", zh)
        self.assertIn("60FPS 解鎖", zh)

    def test_different_master_split(self):
        meta = (self.dist / "db" / C / f"{D}.meta.tsv").read_text(encoding="utf-8").splitlines()
        self.assertEqual([m.split("\t")[0] for m in meta], [f"{D}.txt", f"{D}.alt1.txt"])
        self.assertEqual(self.index["titles"][T]["builds"][B]["groups"], 1)
        self.assertEqual(self.index["titles"][C]["builds"][D]["groups"], 2)

    def test_zip_and_nested_paths(self):
        self.assertIn("範例C", self.report["sources"])
        self.assertTrue(self.report["sources"]["範例C"].startswith("1 個檔"))
        zh = self.zh(E, F)
        self.assertIn("無限血量", zh)          # zip 來源（沒有 master，併進 D 那組）
        self.assertIn("無敵模式", zh)          # 路徑有遊戲名稱的來源

    def test_ips_patch_carried(self):
        g = self.detail(E, F)
        self.assertEqual(len(g["patches"]), 1)
        self.assertTrue((self.dist / "db" / E / g["patches"][0]).exists())
        meta = (self.dist / "db" / E / f"{F}.meta.tsv").read_text(encoding="utf-8").split("\t")
        self.assertEqual(meta[2], "1")
        self.assertTrue(self.index["titles"][E]["builds"][F]["needs_patch"])

    def test_noips_ignored(self):
        self.assertEqual(len(self.detail(E, F)["cheats"]), 2)

    def test_title_names(self):
        self.assertEqual(self.index["titles"][E]["name"], "範例遊戲丙（titledb）")
        self.assertEqual(self.index["titles"][T]["name"], "範例遊戲甲")     # 手動優先

    def test_broken_and_disabled_sources(self):
        self.assertIn("無法取得", self.report["sources"]["壞掉的"])
        self.assertNotIn("關掉的", self.report["sources"])

    def test_output_reparses(self):
        text = (self.dist / "db" / T / f"{B}.txt").read_text(encoding="utf-8")
        master, cheats, warns = mc.parse_cheat_text(text)
        self.assertIsNotNone(master)
        self.assertEqual(len(cheats), 5)
        self.assertEqual(warns, [])

    def test_refuses_foreign_out_dir(self):
        foreign = self.tmp / "foreign"
        foreign.mkdir()
        (foreign / "important.txt").write_text("x")
        rc = mc.main(["--sources", str(HERE / "sources_test.json"), "--out", str(foreign),
                      "--cache", str(self.tmp / "cache")])
        self.assertEqual(rc, 1)
        self.assertTrue((foreign / "important.txt").exists())


if __name__ == "__main__":
    unittest.main()
