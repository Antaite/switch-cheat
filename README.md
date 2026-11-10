# CheatTW 繁中金手指

大氣層（Atmosphere）用的繁體中文金手指工具：

- **Tesla overlay**：遊戲中直接開關碼、切換碼庫組合、記憶體數值搜尋
- **碼庫合併工具**：每天把 12 個開源碼庫合併成一份，重複的碼自動整合

> 專案名稱 CheatTW 是暫定的，之後可以整個換掉。

## 收錄的碼庫

排越前面優先權越高（名稱衝突時以前面的為主），設定在 `tools/sources.json`。

| id | 碼庫 | 種類 | 說明 |
|---|---|---|---|
| tomvita | tomvita/MyNXCheats | git | Breeze 作者，含特殊碼 |
| 60fps | ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats | git | 60FPS、解析度、畫質 |
| bad1dea | bad1dea/NXCheats | git + IPS | MIT 授權，部分遊戲需要 IPS 修補檔，會一起收錄 |
| arch9sk7 | Arch9SK7/MyNXCheats | git | |
| enterprisefreak | EnterpriseFreak/nsw-atmos-cheats | git | |
| edizon | WerWolv/EdiZon_CheatsConfigsAndScripts | git | EdiZon 官方碼庫 |
| hamlet | HamletDuFromage/switch-cheats-db | release zip | GBAtemp 主檔 + 60FPS（AIO Updater 用的） |
| sthetix | sthetix/nx-cheats-db | release zip | 目前最大，含 CheatSlips |
| exploitz86 | exploitz86/switch-cheats-db | release zip | SwitchBlade 用的 |
| costelacnx | CostelaCNX/switch-cheats-db | release zip | |
| ibnux | ibnux/switch-cheat | git | 舊版整合碼庫 |
| othmanead | othmanead/switch-cheats | git | ibnux 分支 |

後面幾個是整合型碼庫，內容大量重複，合併時會自動去重。
**授權**：大部分沒有標示授權，公開發佈前請先徵求作者同意；每條碼都保留來源標記。

路徑一律自動辨識：只要長得像 `…/{TID}/cheats/{BID}.txt` 或 `…/{TID}/{BID}.txt` 就收，
所以 `atmosphere/titles/`、`contents/`、`遊戲名稱/TID/` 這些不同的資料夾結構都能讀。
某個來源抓不到（網址失效、改名）只會在 `report.json` 記一筆，不會影響其他來源。

## 資料夾

```
CheatTW/
├─ common/                 共用核心（overlay 和之後的主程式 .nro 都用這份）
│  ├─ include/dmntcht.h        dmnt:cht 服務 IPC（大氣層金手指引擎）
│  ├─ include/cheat_core.hpp   金手指檔解析、序列化、中文翻譯
│  ├─ include/search_engine.hpp 記憶體搜尋引擎（固定記憶體用量）
│  └─ tests/                   電腦上就能跑的單元測試
├─ overlay/                Tesla overlay（.ovl）
│  ├─ source/main.cpp          所有畫面
│  ├─ source/cheat_session.*   連線管理、開關、載入碼庫組合、修補檔
│  └─ source/search_task.*     背景執行緒跑搜尋、接 dmnt 讀記憶體
├─ data/
│  ├─ translations/zh_TW.tsv   中英對照表（英文<TAB>中文<TAB>分類）
│  └─ titles_zh.tsv            手動補充中文遊戲名（其他從 titledb 自動抓）
├─ tools/
│  ├─ merge_cheats.py          碼庫合併工具
│  ├─ sources.json             碼庫來源清單
│  └─ tests/                   合併測試與範例資料
└─ .github/workflows/          每天自動合併並發佈到 gh-pages
```

## 操作流程（overlay）

```
首頁 ─┬─ 分類（全部／戰鬥／金錢／道具／移動／畫面／其他）→ 開關金手指
      ├─ 全部關閉
      ├─ 線上碼庫 → 選組合載入（主碼不同的碼不能混用，所以分成組合）
      └─ 進階搜尋 → 輸入數值 → 搜尋 → 遊戲裡讓數值變化 → 接著搜尋 → 鎖定數值
```

流暢度的做法：

- 畫面只建一次，數字有變才更新（用 generation 計數判斷），不會每幀重算
- 讀 dmnt 的碼一次只拿 16 條，碼再多也不會卡
- 搜尋在背景執行緒跑，畫面可以照常操作，隨時可以取消
- overlay 隱藏時立刻釋放 dmnt 連線，不跟 Breeze／EdiZon-SE 搶（背景搜尋中例外）

## 進階搜尋的記憶體用量

**固定約 150KB，不會因為搜尋結果多寡而增加。**

| 項目 | 大小 | 說明 |
|---|---|---|
| 讀取區塊 | 64 KB | 每次只從遊戲讀 64KB |
| 候選檔讀寫緩衝 | 2 × 32 KB | 候選位址存在 SD 卡（`switch/CheatTW/search/`），不放記憶體 |
| 區段表 | 16 KB | 最多 1024 段 |
| 背景執行緒 stack | 16 KB | 大緩衝都放全域，stack 不會爆 |
| 結果畫面 | 640 bytes | 只顯示前 40 筆 |

保護機制：

- 第一次搜尋超過 200 萬筆（SD 卡上 32MB）就停下來，請使用者換比較特別的數值，並保留上一次的結果
- 讀不到的記憶體頁面直接跳過，不會報錯
- 只搜可讀寫的區段，不碰 IO、保留區
- 第二次以後的搜尋，同一個 64KB 範圍內的候選只讀一次（測試確認）
- 搜尋時暫停遊戲，數值才不會在中途變動，搜完一定恢復

目前支援：整數 1/2/4/8 bytes、float、double；比較方式：等於、大於、小於、變大了、變小了、有變、沒變。
找到後可以「鎖定為目前數值」、「鎖定為自訂數值」或「只改一次」。鎖定會產生 `[自製]` 金手指，存進大氣層的金手指檔。

還沒做：未知初始值搜尋（要先把記憶體整份存到 SD）、指標搜尋。這兩個先用 Breeze。

## 編譯 overlay

需要 devkitPro（devkitA64 + libnx）。

```bash
cd overlay
git submodule add https://github.com/WerWolv/libtesla libs/libtesla
make
```

輸出 `CheatTW.ovl`，放到 `sdmc:/switch/.overlays/`。
把 `data/translations/zh_TW.tsv` 複製到 `sdmc:/switch/CheatTW/zh_TW.tsv`。

**中文字型**：原版 libtesla 用系統內建字型，中文能不能完整顯示要實機確認。
如果出現方塊字，改用有支援中日韓字型的 libtesla 分支（例如 Ultrahand 維護的版本），換 submodule 就好。

## 電腦上的測試

```bash
# 核心解析與翻譯
cd common/tests
g++ -std=c++17 -I../include test_core.cpp ../source/cheat_core.cpp -o test_core && ./test_core

# 搜尋引擎（假記憶體）
g++ -std=c++17 -I../include test_search.cpp ../source/search_engine.cpp ../source/cheat_core.cpp -o test_search && ./test_search

# 碼庫合併
cd tools
python3 -m unittest tests/test_merge.py
python3 merge_cheats.py --out dist          # 實際抓全部碼庫跑一次（要網路）
python3 merge_cheats.py --out dist --only tomvita 60fps   # 只跑幾個來源
```

## 合併規則

| 情況 | 處理方式 |
|---|---|
| TitleID 相同、BuildID 不同 | 不合併（不同版本位址不同） |
| 碼的內容完全一樣 | 只留一條，`sources` 記錄所有來源 |
| 名稱一樣、內容不同 | 兩條都留，名稱加 `[來源]` |
| master code 不同 | 分成不同組合：`{BID}.txt`（推薦）、`{BID}.alt1.txt`… |
| 沒有 master code 的碼 | 放進每一組 |
| 只有標題沒有內容（分隔線） | 移除 |
| 超過 256 個字組的碼 | dmnt 載不進去，直接略過 |
| 超過 128 條 | 照樣輸出，索引標記 `over_limit` |
| 需要 IPS 修補檔 | 修補檔一起輸出，overlay 載入時自動裝到 `exefs_patches/cheattw_{來源}/`，重開遊戲後生效 |

輸出（`dist/`）：

```
index.json                 總索引（小）：中文／英文遊戲名、每個版本有幾組幾條
db/{TID}/{BID}.txt         標準大氣層格式，EdiZon-SE／Breeze 也能讀
db/{TID}/{BID}.alt1.txt    其他組合
db/{TID}/{BID}.meta.tsv    overlay 用的組合清單（簡單 TSV，不用解析 JSON）
db/{TID}/index.json        這款遊戲的詳細清單（中文名、分類、來源）
db/{TID}/patches/…         IPS 修補檔
report.json                每個來源讀到幾個檔、格式警告
```

App 只需要下載總索引和正在玩的那款遊戲的資料夾。

## 進度

- [x] dmnt:cht IPC、金手指解析、中文翻譯（已測試）
- [x] overlay：分類、開關、全部關閉、碼庫組合切換、修補檔安裝
- [x] 進階搜尋：固定記憶體、背景執行緒、7 種比較、鎖定／改值（引擎已測試）
- [x] 碼庫合併：12 個來源、自動辨識路徑、zip、IPS、中文遊戲名（已用範例資料測試）
- [ ] 實機編譯測試（overlay 只用替身標頭做過語法檢查）
- [ ] 實際跑一次全部碼庫的合併（這邊沒有網路，要在 GitHub Actions 或你的電腦跑）
- [ ] 下載功能（overlay 記憶體不夠跑 curl，交給主程式 .nro）
- [ ] 主程式 .nro：中文搜尋、下載碼庫
- [ ] 擴充中英對照表
- [ ] 未知初始值搜尋、指標搜尋

## 致謝

dmnt:cht 介面參考 Atmosphère 與 EdiZon / EdiZon-SE（WerWolv、tomvita）。
所有碼的著作權屬於原作者，合併後每條碼都保留來源標記。
