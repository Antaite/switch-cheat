// CheatTW — 繁中金手指 Tesla overlay
//
// 畫面：
//   首頁 ─┬─ 分類 → 金手指列表（開關）
//         ├─ 全部關閉
//         ├─ 線上碼庫 → 選組合載入（master code 不同的分成不同組合）
//         └─ 進階搜尋 → 數值編輯 / 搜尋結果 → 鎖定數值、產生金手指碼
//
// 流暢度原則：
//   - 畫面元件只建一次，數字變動用 setValue 更新，不重建整個列表
//   - 用 session().generation() 判斷有沒有變，沒變就什麼都不做
//   - 記憶體搜尋在背景執行緒跑，畫面照常操作，可以隨時取消
#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <cstdlib>
#include <cstring>
#include <functional>
#include "cheat_session.hpp"
#include "search_task.hpp"
#include "downloader.hpp"
#include "ui.hpp"

using namespace cheattw;

static const char *kCategories[] = { "全部", "戰鬥", "金錢", "道具", "移動", "畫面", "其他" };
constexpr int kCategoryCount = sizeof(kCategories) / sizeof(kCategories[0]);

// 錯誤碼用 Switch 慣用的 2XXX-YYYY 格式，回報問題時好查
static std::string errorCode(Result rc) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u-%04u", 2000 + (rc & 0x1FF), (rc >> 9) & 0x1FFF);
    return buf;
}

static void mainStatus(std::string &text, ui::Tone &tone) {
    auto &s = session();
    if (!s.isOpen()) {
        text = "dmnt 連不上（錯誤 " + errorCode(s.lastError()) + "）";
        tone = ui::Tone::Error;
    } else if (!s.hasGame()) {
        text = "目前沒有執行中的遊戲";
        tone = ui::Tone::Warn;
    } else if (s.cheats().empty()) {
        text = "這個遊戲版本還沒有金手指";
        tone = ui::Tone::Warn;
    } else {
        text = "dmnt 已連線 · 已開啟 " + std::to_string(s.enabledCount()) + " / " +
               std::to_string(s.cheats().size()) + " 條";
        tone = ui::Tone::Ok;
    }
}

// 只要 A 鍵的點擊處理
template <typename F>
static void onA(tsl::elm::ListItem *item, F fn) {
    item->setClickListener([fn](u64 keys) {
        if (!(keys & HidNpadButton_A)) return false;
        fn();
        return true;
    });
}

// ═══════════════ 數值輸入：數字鍵盤 ═══════════════
class GuiValueEdit : public tsl::Gui {
public:
    GuiValueEdit(std::string title, std::string initial, bool allowFloat,
                 std::function<void(const std::string &)> done)
        : title_(std::move(title)), initial_(std::move(initial)), allowFloat_(allowFloat), done_(std::move(done)) {}

    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame(title_, "用十字鍵選數字按 A，或直接點螢幕");
        frame->setContent(new ui::Keypad(initial_, allowFloat_, done_));
        return frame;
    }

private:
    std::string title_, initial_;
    bool allowFloat_;
    std::function<void(const std::string &)> done_;
};

static bool parseValue(ValueType t, const std::string &s, u64 &out) {
    if (s.empty() || s == "-" || s == ".") return false;
    char *end = nullptr;
    if (t == ValueType::F32 || t == ValueType::F64) {
        double v = std::strtod(s.c_str(), &end);
        if (*end) return false;
        out = encodeValue(t, v);
    } else if (s[0] == '-') {
        long long v = std::strtoll(s.c_str(), &end, 10);
        if (*end) return false;
        out = encodeValue(t, (double)v);
    } else {
        unsigned long long v = std::strtoull(s.c_str(), &end, 10);
        if (*end) return false;
        const size_t w = valueSize(t);
        out = w == 8 ? v : (v & ((1ull << (w * 8)) - 1));
    }
    return true;
}

// ═══════════════ 進階功能共用 ═══════════════
static bool parseDouble(const std::string &s, double &out) {
    if (s.empty() || s == "-" || s == ".") return false;
    char *end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

static bool isFloatType(ValueType t) { return t == ValueType::F32 || t == ValueType::F64; }

// UI 直接讀記憶體用（不需要區段表）
struct DirectReader : MemoryReader {
    size_t regions(Region *, size_t) override { return 0; }
    bool read(uint64_t addr, void *buf, size_t size) override {
        return R_SUCCEEDED(dmntchtReadCheatProcessMemory(addr, buf, size));
    }
};

// 背景工作的進度文字
static std::string jobProgress() {
    auto &t = searchTask();
    const u64 total = t.progress().total, done = t.progress().done;
    const int pct = total ? (int)(done * 100 / total) : 0;
    std::string what, detail;
    switch (t.runningJob()) {
        case JobKind::Scan:
            what = "搜尋中";
            detail = t.engine().started() && !t.engine().inSnapshot()
                ? std::to_string(done) + " / " + std::to_string(total) + " 筆"
                : std::to_string(done >> 20) + " / " + std::to_string(total >> 20) + " MB";
            break;
        case JobKind::PointerSearch: what = "指標搜尋中"; detail = std::to_string(done >> 20) + " / " + std::to_string(total >> 20) + " MB"; break;
        case JobKind::PointerFilter: what = "篩選指標中"; detail = std::to_string(done) + " / " + std::to_string(total) + " 條"; break;
        case JobKind::Port:          what = "移植中"; detail = std::to_string(done) + " / " + std::to_string(total) + " 條"; break;
        default: return "";
    }
    return what + " " + std::to_string(pct) + "%（" + detail + "）";
}

// ═══════════════ 指標：單一路徑的操作 ═══════════════
class GuiPtrAction : public tsl::Gui {
public:
    explicit GuiPtrAction(PtrChain c) : chain_(c) {}

    tsl::elm::Element *createUI() override {
        enc_ = searchTask().pointerEncoding();
        DirectReader rd;
        u64 addr = 0;
        cur_ = 0;
        const bool ok = PointerSearch::resolve(rd, session().extents().mainBase, chain_, addr) &&
                        rd.read(addr, &cur_, valueSize(enc_.type));
        auto frame = new tsl::elm::OverlayFrame(PointerSearch::describe(chain_),
                                                ok ? "目前數值 " + formatEncoded(enc_, cur_) : "這條路徑現在讀不到");
        auto list = new tsl::elm::List();
        list->addItem(new ui::StatusBar([this](std::string &t, ui::Tone &tone) {
            t = msg_.empty() ? "產生的金手指重開遊戲也能用" : msg_;
            tone = msgTone_;
        }));
        lockNow_ = new tsl::elm::ListItem("鎖定為目前數值");
        onA(lockNow_, [this, ok]() { if (ok) make(cur_); else setMsg("這條路徑現在讀不到", false); });
        list->addItem(lockNow_);
        lockCustom_ = new tsl::elm::ListItem("鎖定為自訂數值…");
        onA(lockCustom_, [this]() {
            tsl::changeTo<GuiValueEdit>("鎖定數值", formatEncoded(enc_, cur_), isFloatType(enc_.type) || enc_.scale > 1,
                [this](const std::string &s) {
                    double d;
                    if (parseDouble(s, d)) make(encodeDisplay(enc_, d)); else setMsg("數值格式不對", false);
                });
        });
        list->addItem(lockCustom_);
        frame->setContent(new ui::SplitView(list, new ui::HelpBox([this]() -> std::string {
            auto *f = getFocusedElement();
            if (f == lockNow_) return "產生一條指標金手指，讓這個數值一直維持現在的值。重開遊戲也有效，可以分享給別人。";
            if (f == lockCustom_) return "先輸入想要的數字（例如金錢 999999），再產生指標金手指一直維持這個值。";
            return "路徑的意思：從主程式固定位置出發，每個「→ +數字」是讀一次指標再往後加的偏移。";
        }), 170));
        return frame;
    }

private:
    void setMsg(const std::string &m, bool ok) { msg_ = m; msgTone_ = ok ? ui::Tone::Ok : ui::Tone::Error; }
    void make(u64 raw) {
        Cheat c = PointerSearch::makeCheat("[指標] 鎖定 " + formatEncoded(enc_, raw), chain_, enc_.type, raw);
        std::string m;
        const Result rc = session().addOwnCheat(c, true, m);
        setMsg(m, R_SUCCEEDED(rc));
    }

    PtrChain chain_;
    Encoding enc_{ValueType::U32, 1};
    u64 cur_ = 0;
    tsl::elm::ListItem *lockNow_ = nullptr, *lockCustom_ = nullptr;
    std::string msg_;
    ui::Tone msgTone_ = ui::Tone::Ok;
};

// ═══════════════ 指標：結果列表 ═══════════════
class GuiPtrResults : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto &task = searchTask();
        const size_t total = task.pointerCount();
        const Encoding enc = task.pointerEncoding();
        auto frame = new tsl::elm::OverlayFrame("指標搜尋結果", "共 " + std::to_string(total) + " 條");
        auto list = new tsl::elm::List();
        static PtrChain buf[kShow];
        const size_t n = PointerSearch::read(task.pointerPath(), buf, kShow, 0);
        DirectReader rd;
        for (size_t i = 0; i < n; ++i) {
            u64 addr = 0, v = 0;
            const bool ok = PointerSearch::resolve(rd, session().extents().mainBase, buf[i], addr) &&
                            rd.read(addr, &v, valueSize(enc.type));
            auto item = new tsl::elm::ListItem(PointerSearch::describe(buf[i]), ok ? formatEncoded(enc, v) : "讀不到");
            const PtrChain c = buf[i];
            onA(item, [c]() { tsl::changeTo<GuiPtrAction>(c); });
            list->addItem(item);
        }
        if (n == 0) list->addItem(new tsl::elm::ListItem("沒有找到指標路徑"));
        const bool many = total > 20;
        frame->setContent(new ui::SplitView(list, new ui::HelpBox([many]() {
            return std::string("每條是一個從主程式到數值的路徑，右邊是現在讀到的值。層數越少越穩定，建議選上面幾條。") +
                   (many ? "\n路徑還很多：重開遊戲後重新搜到這個數值，在結果上選「用這個位址篩選指標」，剩下的就是每次都有效的路徑。" : "");
        }), 200));
        return frame;
    }

private:
    static constexpr size_t kShow = 40;
};

// ═══════════════ 進階搜尋：單一結果的操作 ═══════════════
class GuiResultAction : public tsl::Gui {
public:
    GuiResultAction(u64 addr, u64 current, Encoding enc) : addr_(addr), current_(current), enc_(enc) {}

    tsl::elm::Element *createUI() override {
        auto &task = searchTask();
        u8 region = 1;
        u64 offset = 0;
        std::string where;
        const bool known = task.locate(addr_, region, offset, where);

        auto frame = new tsl::elm::OverlayFrame(known ? where : "未知區段",
                                                "目前數值 " + formatEncoded(enc_, current_) + "（" + encodingName(enc_) + "）");
        auto list = new tsl::elm::List();
        list->addItem(new ui::StatusBar([this](std::string &t, ui::Tone &tone) {
            if (searchTask().running()) { t = jobProgress(); tone = ui::Tone::Warn; return; }
            t = msg_.empty() ? "選擇要怎麼處理這個位址" : msg_;
            tone = msgTone_;
        }));

        const bool allowDot = isFloatType(enc_.type) || enc_.scale > 1;
        if (known) {
            lockNow_ = new tsl::elm::ListItem("鎖定為目前數值");
            onA(lockNow_, [this, region, offset, where]() { makeLock(region, offset, where, current_); });
            list->addItem(lockNow_);

            lockCustom_ = new tsl::elm::ListItem("鎖定為自訂數值…");
            onA(lockCustom_, [this, region, offset, where, allowDot]() {
                tsl::changeTo<GuiValueEdit>("鎖定數值", formatEncoded(enc_, current_), allowDot,
                    [this, region, offset, where](const std::string &s) {
                        double d;
                        if (parseDouble(s, d)) makeLock(region, offset, where, encodeDisplay(enc_, d));
                        else setMsg("數值格式不對", false);
                    });
            });
            list->addItem(lockCustom_);
        }

        writeOnce_ = new tsl::elm::ListItem("只改一次（不鎖定）…");
        onA(writeOnce_, [this, allowDot]() {
            tsl::changeTo<GuiValueEdit>("改成", formatEncoded(enc_, current_), allowDot, [this](const std::string &s) {
                double d;
                if (!parseDouble(s, d)) { setMsg("數值格式不對", false); return; }
                const u64 bits = encodeDisplay(enc_, d);
                const Result rc = dmntchtWriteCheatProcessMemory(addr_, &bits, valueSize(enc_.type));
                setMsg(R_SUCCEEDED(rc) ? "已寫入" : "寫入失敗", R_SUCCEEDED(rc));
            });
        });
        list->addItem(writeOnce_);

        list->addItem(new tsl::elm::CategoryHeader("重開遊戲也能用"));
        ptrSearch_ = new tsl::elm::ListItem("指標搜尋", "約 1～3 分鐘");
        onA(ptrSearch_, [this]() {
            std::string why;
            if (!searchTask().startPointerSearch(addr_, enc_, &why)) setMsg("無法開始：" + why, false);
        });
        list->addItem(ptrSearch_);

        ptrFilter_ = new tsl::elm::ListItem("用這個位址篩選指標", searchTask().pointerCount() ? std::to_string(searchTask().pointerCount()) + " 條" : "還沒有");
        onA(ptrFilter_, [this]() {
            std::string why;
            if (!searchTask().startPointerFilter(addr_, &why)) setMsg("無法開始：" + why, false);
        });
        list->addItem(ptrFilter_);

        ptrView_ = new tsl::elm::ListItem("查看指標結果");
        onA(ptrView_, []() { if (searchTask().pointerCount() && !searchTask().running()) tsl::changeTo<GuiPtrResults>(); });
        list->addItem(ptrView_);

        frame->setContent(new ui::SplitView(list, new ui::HelpBox([this]() { return help(); }), 190));
        return frame;
    }

    void update() override {
        if (!searchTask().takeFinished()) return;
        const SearchResult r = searchTask().lastResult();
        setMsg(r == SearchResult::Ok ? searchTask().summary() : std::string(searchResultText(r)), r == SearchResult::Ok);
        ptrFilter_->setValue(std::to_string(searchTask().pointerCount()) + " 條");
    }

private:
    std::string help() {
        auto *f = getFocusedElement();
        if (f == lockNow_) return "產生一條金手指，讓這個位址一直維持現在的值（例如血量不會再減少）。名稱開頭是 [自製]。";
        if (f == lockCustom_) return "先輸入想要的數字（例如金錢 999999），再產生金手指一直維持這個值。";
        if (f == writeOnce_) return "直接改成你輸入的值，不產生金手指；遊戲之後還是會照常改變它。";
        if (f == ptrSearch_) return "一般鎖定的位址每次開遊戲都會變。指標搜尋會找出「從主程式出發、經過幾層指標到這裡」的路徑，產生的碼重開遊戲也能用。搜尋時遊戲會暫停。";
        if (f == ptrFilter_) return "指標路徑通常會找到很多條。重開遊戲後重新搜到同一個數值的新位址，在這裡篩選：只留下仍然指到新位址的路徑。";
        if (f == ptrView_) return "看目前存著的指標路徑，選一條產生金手指。";
        return "選擇要怎麼處理這個位址。";
    }
    void setMsg(const std::string &m, bool ok) { msg_ = m; msgTone_ = ok ? ui::Tone::Ok : ui::Tone::Error; }
    void makeLock(u8 region, u64 offset, const std::string &where, u64 bits) {
        Cheat c = makeStaticWriteCheat("[自製] 鎖定 " + where + " = " + formatEncoded(enc_, bits), region, offset, enc_.type, bits);
        std::string m;
        const Result rc = session().addOwnCheat(c, true, m);
        setMsg(m, R_SUCCEEDED(rc));
    }

    u64 addr_, current_;
    Encoding enc_;
    tsl::elm::ListItem *lockNow_ = nullptr, *lockCustom_ = nullptr, *writeOnce_ = nullptr;
    tsl::elm::ListItem *ptrSearch_ = nullptr, *ptrFilter_ = nullptr, *ptrView_ = nullptr;
    std::string msg_;
    ui::Tone msgTone_ = ui::Tone::Ok;
};

// ═══════════════ 進階搜尋：結果列表（Y 試改） ═══════════════
class GuiResults : public tsl::Gui {
public:
    ~GuiResults() override {
        // 離開畫面時，試改過的全部改回原值
        for (size_t i = 0; i < n_; ++i)
            if (trying_[i]) dmntchtWriteCheatProcessMemory(candAddr(buf_[i]), &orig_[i], valueSize(encOf(i).type));
    }

    tsl::elm::Element *createUI() override {
        auto &task = searchTask();
        auto &eng = task.engine();
        auto frame = new tsl::elm::OverlayFrame("搜尋結果", "共 " + std::to_string(eng.count()) + " 筆，顯示前 " +
                                                std::to_string(std::min<u64>(eng.count(), kShow)) + " 筆");
        auto list = new tsl::elm::List();
        n_ = eng.peek(buf_, kShow, 0);
        DirectReader rd;
        for (size_t i = 0; i < n_; ++i) {
            const Encoding e = encOf(i);
            const u64 addr = candAddr(buf_[i]);
            cur_[i] = 0;
            ok_[i] = rd.read(addr, &cur_[i], valueSize(e.type));
            trying_[i] = false;
            std::string where;
            u8 r; u64 off;
            if (!task.locate(addr, r, off, where)) {
                char tmp[24];
                std::snprintf(tmp, sizeof(tmp), "%llX", (unsigned long long)addr);
                where = tmp;
            }
            items_[i] = new tsl::elm::ListItem(where, label(i));
            items_[i]->setClickListener([this, i](u64 keys) {
                if (keys & HidNpadButton_A) {
                    tsl::changeTo<GuiResultAction>(candAddr(buf_[i]), cur_[i], encOf(i));
                    return true;
                }
                if (keys & HidNpadButton_Y) { toggleTry(i); return true; }
                return false;
            });
            list->addItem(items_[i]);
        }
        if (n_ == 0) list->addItem(new tsl::elm::ListItem("沒有結果"));
        tooMany_ = eng.count() > kShow;
        frame->setContent(new ui::SplitView(list, new ui::HelpBox([this]() { return help(); }), 200));
        return frame;
    }

private:
    static constexpr size_t kShow = 40;

    Encoding encOf(size_t i) const { return searchTask().engine().encoding(candEnc(buf_[i])); }

    std::string label(size_t i) const {
        if (!ok_[i]) return "讀不到";
        const Encoding e = encOf(i);
        std::string v = trying_[i] ? "試改中 " : "";
        v += formatEncoded(e, cur_[i]);
        if (searchTask().engine().mode() == SearchMode::Smart) v += "・" + encodingName(e);
        return v;
    }

    // Y：暫時改成 999（本來就是 999 就改 777），回遊戲看畫面數字有沒有跟著變；再按一次改回來
    void toggleTry(size_t i) {
        if (!ok_[i]) return;
        const Encoding e = encOf(i);
        const u64 addr = candAddr(buf_[i]);
        const size_t w = valueSize(e.type);
        if (trying_[i]) {
            dmntchtWriteCheatProcessMemory(addr, &orig_[i], w);
            cur_[i] = orig_[i];
            trying_[i] = false;
        } else {
            orig_[i] = cur_[i];
            u64 test = encodeDisplay(e, 999);
            if (test == cur_[i]) test = encodeDisplay(e, 777);
            if (R_SUCCEEDED(dmntchtWriteCheatProcessMemory(addr, &test, w))) { cur_[i] = test; trying_[i] = true; }
        }
        items_[i]->setValue(label(i));
    }

    std::string help() {
        std::string h = "按 Y「試改」：暫時改成 999，回遊戲看畫面數字有沒有跟著變，有變就是它；再按一次 Y 改回原值（離開這頁也會自動改回）。按 A 鎖定或做指標搜尋。";
        auto *f = getFocusedElement();
        for (size_t i = 0; i < n_; ++i) {
            if (f != items_[i]) continue;
            int32_t offs[2];
            DirectReader rd;
            const int k = searchTask().engine().groupNeighbors(rd, candAddr(buf_[i]), offs);
            if (k > 0) {
                h += "\n群組：";
                for (int j = 0; j < k; ++j) h += (j ? "、" : "") + std::string(offs[j] > 0 ? "+" : "") + std::to_string(offs[j]) + " bytes 處有第 " + std::to_string(j + 2) + " 個數值";
            }
        }
        if (tooMany_) h += "\n結果還很多，建議回上一頁，在遊戲裡讓數值變化後再搜一次。";
        return h;
    }

    Candidate buf_[kShow];
    u64 cur_[kShow] = {}, orig_[kShow] = {};
    bool ok_[kShow] = {}, trying_[kShow] = {};
    tsl::elm::ListItem *items_[kShow] = {};
    size_t n_ = 0;
    bool tooMany_ = false;
};

// ═══════════════ 進階搜尋：主畫面 ═══════════════
static SearchSettings g_ui;   // 數值存成 char[32]，可以直接存檔

class GuiSearch : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        // overlay 關掉再打開：接回上次的搜尋進度
        const RestoreResult rr = searchTask().restore(g_ui);
        if (rr == RestoreResult::Restored)
            setStatus("已接回上次的搜尋（" + std::to_string(searchTask().engine().count()) + " 筆），可以接著搜尋");
        else if (rr == RestoreResult::RestoredStale)
            setStatus("已接回上次的搜尋，但遊戲重開過，位址可能變了，建議「重新開始」", ui::Tone::Warn);

        auto frame = new tsl::elm::OverlayFrame("進階搜尋", "記憶體數值搜尋");
        auto list = new tsl::elm::List();
        list->addItem(new ui::StatusBar([this](std::string &t, ui::Tone &tone) {
            if (searchTask().running()) { t = jobProgress(); tone = ui::Tone::Warn; return; }
            t = statusText_;
            tone = statusTone_;
        }));

        list->addItem(new tsl::elm::CategoryHeader("條件"));
        mode_ = item("搜尋模式", [this]() {
            if (locked()) return;
            g_ui.mode = (SearchMode)(((int)g_ui.mode + 1) % 4);
            if (g_ui.mode == SearchMode::Unknown && !isFloatType(g_ui.type)) g_ui.type = ValueType::F32;   // 血條多半是小數
            g_ui.cond = Cond::Equal;
        }, list);
        type_ = item("資料型別", [this]() {
            if (locked() || g_ui.mode == SearchMode::Smart) return;
            g_ui.type = (ValueType)(((int)g_ui.type + 1) % 6);
        }, list);
        cond_ = item("比較方式", [this]() {
            const auto conds = allowedConds();
            if (conds.size() <= 1) return;
            size_t i = 0;
            while (i < conds.size() && conds[i] != g_ui.cond) ++i;
            g_ui.cond = conds[(i + 1) % conds.size()];
        }, list);
        value_ = valueItem("數值", g_ui.value, sizeof(g_ui.value), list);
        value2_ = valueItem("第二個數值", g_ui.value2, sizeof(g_ui.value2), list);
        value3_ = valueItem("第三個數值", g_ui.value3, sizeof(g_ui.value3), list);
        scope_ = item("範圍", [this]() {
            if (locked()) return;
            g_ui.scope = (SearchScope)(((int)g_ui.scope + 1) % 3);
        }, list);

        list->addItem(new tsl::elm::CategoryHeader("動作"));
        action_ = item("", [this]() { onAction(); }, list);
        results_ = item("查看結果", []() {
            auto &e = searchTask().engine();
            if (e.started() && !e.inSnapshot() && !searchTask().running()) tsl::changeTo<GuiResults>();
        }, list);
        ptr_ = item("指標搜尋結果", []() {
            if (searchTask().pointerCount() && !searchTask().running()) tsl::changeTo<GuiPtrResults>();
        }, list);
        reset_ = item("重新開始", [this]() {
            searchTask().reset();
            g_ui.cond = Cond::Equal;
            setStatus("已清除，可以重新選模式");
        }, list);

        refreshLabels();
        frame->setContent(new ui::SplitView(list, new ui::HelpBox([this]() { return helpText(); }), 200));
        return frame;
    }

    void update() override {
        auto &task = searchTask();
        if (task.takeFinished()) {
            const SearchResult r = task.lastResult();
            if (task.lastJob() == JobKind::Scan) {
                if (r == SearchResult::Ok) {
                    task.persist(g_ui);   // 存檔，退出再進來也能接著搜
                    auto &e = task.engine();
                    if (e.inSnapshot())
                        setStatus("還有 " + std::to_string(e.count()) + " 個可能位置（" + task.summary() + "）。在遊戲裡讓它變化，再選「變大了／變小了」接著篩", ui::Tone::Ok);
                    else
                        setStatus("找到 " + std::to_string(e.count()) + " 筆（" + task.summary() + "）" +
                                  (e.count() > 0 && e.count() <= 10 ? "，可以查看結果並按 Y 試改" : ""));
                } else {
                    setStatus(std::string(searchResultText(r)) + "（" + task.summary() + "）", ui::Tone::Error);
                }
            } else {
                setStatus(r == SearchResult::Ok ? task.summary() : std::string(searchResultText(r)),
                          r == SearchResult::Ok ? ui::Tone::Ok : ui::Tone::Error);
            }
        }
        if (++tick_ % 15 == 0) refreshLabels();   // 數字有變時更新（不用每幀）
    }

private:
    template <typename F>
    tsl::elm::ListItem *item(const std::string &text, F fn, tsl::elm::List *list) {
        auto *it = new tsl::elm::ListItem(text, "");
        onA(it, [this, fn]() { fn(); refreshLabels(); });
        list->addItem(it);
        return it;
    }

    tsl::elm::ListItem *valueItem(const std::string &text, char *dst, size_t cap, tsl::elm::List *list) {
        auto *it = new tsl::elm::ListItem(text, "");
        onA(it, [this, it, dst, cap]() {
            if (!valueEnabled(it)) return;
            const bool f = g_ui.mode == SearchMode::Smart || isFloatType(g_ui.type);
            tsl::changeTo<GuiValueEdit>(text_of(it), std::string(dst), f, [this, dst, cap](const std::string &s) {
                std::snprintf(dst, cap, "%s", s.c_str());
                refreshLabels();
            });
        });
        list->addItem(it);
        return it;
    }

    std::string text_of(tsl::elm::ListItem *it) const {
        return it == value2_ ? "第二個數值" : it == value3_ ? "第三個數值" : "搜尋數值";
    }

    bool locked() const { return searchTask().engine().started() || searchTask().running(); }
    bool started() const { return searchTask().engine().started(); }

    std::vector<Cond> allowedConds() const {
        if (started()) return {Cond::Equal, Cond::Greater, Cond::Less, Cond::Increased, Cond::Decreased, Cond::Changed, Cond::Unchanged};
        switch (g_ui.mode) {
            case SearchMode::Normal:  return {Cond::Equal, Cond::Greater, Cond::Less};
            case SearchMode::Unknown: return {};
            default:                  return {Cond::Equal};
        }
    }

    bool valueEnabled(tsl::elm::ListItem *it) const {
        if (it == value_) return started() ? condNeedsValue(g_ui.cond) : (g_ui.mode != SearchMode::Unknown);
        return !started() && g_ui.mode == SearchMode::Group;   // 第二、第三個數值只有群組第一次用
    }

    void onAction() {
        auto &task = searchTask();
        if (task.running()) { task.cancel(); return; }
        ScanSpec spec;
        spec.mode = g_ui.mode;
        spec.type = g_ui.type;
        spec.cond = g_ui.cond;
        const bool first = !started();
        if ((first && g_ui.mode != SearchMode::Unknown) || (!first && condNeedsValue(g_ui.cond))) {
            if (!parseDouble(g_ui.value, spec.value)) { setStatus("數值格式不對", ui::Tone::Error); return; }
        }
        if (first && g_ui.mode == SearchMode::Group) {
            spec.extraCount = 0;
            for (const char *v : {g_ui.value2, g_ui.value3}) {
                double d;
                if (v[0] && parseDouble(v, d)) spec.extra[spec.extraCount++] = d;
            }
            if (spec.extraCount == 0) { setStatus("群組搜尋至少要輸入第二個數值", ui::Tone::Error); return; }
        }
        std::string why;
        if (!task.startScan(first, spec, g_ui.scope, &why)) setStatus("無法開始：" + why, ui::Tone::Error);
    }

    void refreshLabels() {
        auto &task = searchTask();
        auto &e = task.engine();
        const bool st = started();
        mode_->setValue(modeName(g_ui.mode), locked());
        type_->setValue(g_ui.mode == SearchMode::Smart ? "自動（不用選）" : valueTypeName(g_ui.type),
                        locked() || g_ui.mode == SearchMode::Smart);
        const auto conds = allowedConds();
        if (!conds.empty() && std::find(conds.begin(), conds.end(), g_ui.cond) == conds.end()) g_ui.cond = conds[0];
        cond_->setValue(conds.empty() ? "第一次不用" : condName(g_ui.cond), conds.size() <= 1);
        value_->setValue(valueEnabled(value_) ? std::string(g_ui.value) : "（不用）", !valueEnabled(value_));
        value2_->setValue(valueEnabled(value2_) ? (g_ui.value2[0] ? std::string(g_ui.value2) : "按 A 輸入") : "（群組才用）", !valueEnabled(value2_));
        value3_->setValue(valueEnabled(value3_) ? (g_ui.value3[0] ? std::string(g_ui.value3) : "可不填") : "（群組才用）", !valueEnabled(value3_));
        static const char *scopeName[] = {"遊戲資料（建議）", "＋主程式", "全部記憶體"};
        scope_->setValue(scopeName[(int)g_ui.scope % 3], locked());
        action_->setText(task.running() ? "取消" : !st ? (g_ui.mode == SearchMode::Unknown ? "記下目前記憶體" : "開始搜尋") : "接著搜尋");
        if (!st) results_->setValue("—", true);
        else if (e.inSnapshot()) results_->setValue("還太多", true);
        else results_->setValue(std::to_string(e.count()) + " 筆");
        const size_t pc = task.pointerCount();
        ptr_->setValue(pc ? std::to_string(pc) + " 條" : "—", pc == 0);
        if (statusText_.empty())
            setStatus(st ? "在遊戲裡讓數值變化後，再接著搜尋" : "選好模式，輸入遊戲裡看到的數字，按「開始搜尋」");
    }

    void setStatus(const std::string &t, ui::Tone tone = ui::Tone::Ok) { statusText_ = t; statusTone_ = tone; }

    // 說明框內容：依游標所在的選項，說明用途和目前選的值代表什麼
    std::string helpText() {
        auto *f = getFocusedElement();
        const bool st = started();
        if (f == mode_) {
            static const char *desc[] = {
                "一般：自己指定資料型別搜尋。",
                "智慧（推薦）：不用選型別，同時試整數、float、double、×10、×100 等各種存法，結果會標出是哪一種。",
                "群組：輸入 2～3 個畫面上看得到、而且相關的數字（例如血量 120、最大血量 150），只找彼此靠在一起的位置，常常一次就找到。",
                "未知數值：給血條、體力條這類沒有數字的東西。先記下整份記憶體，之後選「變大了／變小了／沒變」慢慢縮小範圍。",
            };
            return std::string("搜尋模式：按 A 切換，第一次搜尋後就不能改。\n目前：") + desc[(int)g_ui.mode];
        }
        if (f == type_) {
            if (g_ui.mode == SearchMode::Smart) return "智慧模式會自動試各種存法，不用選型別。";
            static const char *desc[] = {
                "整數 1 byte（0～255）：等級、道具數量這類小數字。",
                "整數 2 bytes（0～65535）：中等大小的數字。",
                "整數 4 bytes：最常用，血量、金錢、經驗值大多是這種。",
                "整數 8 bytes：非常大的數字，少見。",
                "小數 float：有小數點的值，例如移動速度、計時器、血條比例。",
                "小數 double：精度更高的小數，少見。",
            };
            return std::string("資料型別：按 A 切換。\n目前：") + desc[(int)g_ui.type];
        }
        if (f == cond_) {
            if (!st && g_ui.mode == SearchMode::Unknown) return "未知數值的第一次只是把記憶體記下來，不用比較。";
            static const char *desc[] = {
                "等於：找跟輸入數值一樣的位址。",
                "大於：找比輸入數值大的位址。",
                "小於：找比輸入數值小的位址。",
                "變大了：跟上次比變大了（例如撿到錢之後）。不用輸入數值。",
                "變小了：跟上次比變小了（例如被打扣血之後）。不用輸入數值。",
                "有變：跟上次比有任何變化。",
                "沒變：跟上次比完全沒變，用來排除會亂跳的位址。",
            };
            return std::string("比較方式：按 A 切換。\n目前：") + desc[(int)g_ui.cond] +
                   (st ? "" : g_ui.mode == SearchMode::Normal ? "\n搜過一次後才有「變大了」這些。" : "\n這個模式第一次只能用「等於」。");
        }
        if (f == value_) return valueEnabled(value_) ? "數值：輸入遊戲畫面上看到的數字。按 A 開啟數字鍵盤。" : "數值：目前不需要輸入。";
        if (f == value2_ || f == value3_)
            return "群組搜尋用：再輸入 1～2 個跟第一個數值相關、畫面上看得到的數字（例如最大血量、魔力）。程式只找這幾個數字靠在一起的位置。";
        if (f == scope_) {
            static const char *desc[] = {
                "遊戲資料（Heap＋Alias）：血量、金錢幾乎都在這裡，大部分情況用這個。",
                "再加上主程式：少數遊戲把數值放在主程式裡，遊戲資料找不到時再試。",
                "全部可讀寫的記憶體：最完整但最慢，前兩個都找不到時才用。",
            };
            return std::string("範圍：按 A 切換，第一次搜尋後就不能改。\n目前：") + desc[(int)g_ui.scope % 3];
        }
        if (f == action_) {
            if (searchTask().running()) return "取消目前的工作，上一次的結果會保留。";
            if (!st && g_ui.mode == SearchMode::Unknown) return "記下目前記憶體：存一份到 SD 卡（可能要幾百 MB，會先確認空間）。之後在遊戲裡讓血條變化，再選「變大了／變小了」接著搜尋。";
            return st ? "接著搜尋：在遊戲裡讓數值變化後按這裡，從上次的結果裡再篩一次。" : "開始搜尋：搜尋時遊戲會暫停，搜完自動繼續。結果存在 SD 卡，退出再進來也能接著搜。";
        }
        if (f == results_) return "查看結果：列出找到的位址（最多 40 筆）。按 Y 可以「試改」確認是哪一個，按 A 鎖定或做指標搜尋。";
        if (f == ptr_) return "指標搜尋的結果：從主程式到數值的路徑，產生的碼重開遊戲也能用。在搜尋結果上按 A →「指標搜尋」產生。";
        if (f == reset_) return "重新開始：清掉目前的結果，換模式、數值或型別時用。";
        return "建議：先用「智慧」模式輸入畫面上的數字；有兩三個相關數字時用「群組」最快；沒有數字的血條用「未知數值」。";
    }

    tsl::elm::ListItem *mode_ = nullptr, *type_ = nullptr, *cond_ = nullptr, *value_ = nullptr, *value2_ = nullptr,
                       *value3_ = nullptr, *scope_ = nullptr, *action_ = nullptr, *results_ = nullptr, *ptr_ = nullptr,
                       *reset_ = nullptr;
    std::string statusText_;
    ui::Tone statusTone_ = ui::Tone::Ok;
    u32 tick_ = 0;
};

// ═══════════════ 跨版本移植 ═══════════════
// 流程：下載其他版本的碼 → 在你的版本找新起點（投票）→ 列出結果，選要加入的
class GuiPort : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("跨版本移植", "用其他版本的碼");
        list_ = new tsl::elm::List();
        list_->addItem(new ui::StatusBar([this](std::string &t, ui::Tone &tone) {
            if (downloader().running()) { t = downloader().message(); tone = ui::Tone::Warn; return; }
            if (searchTask().runningJob() == JobKind::Port) { t = jobProgress(); tone = ui::Tone::Warn; return; }
            t = msg_;
            tone = tone_;
        }));
        start_ = new tsl::elm::ListItem("開始移植", "下載＋分析");
        onA(start_, [this]() { begin(); });
        list_->addItem(start_);
        all_ = new tsl::elm::ListItem("加入全部信心高的碼");
        onA(all_, [this]() { addAll(); });
        list_->addItem(all_);
        list_->addItem(new tsl::elm::CategoryHeader("結果（按 A 加入；加入後預設關閉，到首頁開啟）", true));

        msg_ = "你的版本沒有碼時，可以把其他版本的「指標碼」移植過來";
        // 之前做過就直接顯示結果
        if (!searchTask().portResults().empty()) { showResults(); msg_ = searchTask().summary(); }

        frame->setContent(new ui::SplitView(list_, new ui::HelpBox([this]() { return help(); }), 210));
        return frame;
    }

    void update() override {
        if (downloader().takeFinished()) {
            if (downloader().state() == Downloader::State::Done) {
                std::string why;
                if (!searchTask().startPort(&why)) setMsg("無法開始：" + why, ui::Tone::Error);
            } else {
                setMsg(downloader().message(), downloader().state() == Downloader::State::NotFound ? ui::Tone::Warn : ui::Tone::Error);
            }
        }
        if (searchTask().lastJob() == JobKind::Port && searchTask().takeFinished()) {
            const SearchResult r = searchTask().lastResult();
            setMsg(r == SearchResult::Ok ? searchTask().summary() : std::string(searchResultText(r)) + "：" + searchTask().summary(),
                   r == SearchResult::Ok ? ui::Tone::Ok : ui::Tone::Error);
            showResults();
        }
    }

private:
    void setMsg(const std::string &m, ui::Tone t) { msg_ = m; tone_ = t; }

    void begin() {
        if (downloader().running() || searchTask().running()) return;
        if (!downloader().startOthers(session().tid(), session().bid())) setMsg("無法開始下載", ui::Tone::Error);
    }

    void showResults() {
        auto *focused = getFocusedElement();
        for (auto *e : items_) if (e == focused) { removeFocus(); break; }
        for (auto *e : items_) list_->removeItem(e);
        items_.clear();
        const auto &res = searchTask().portResults();
        for (size_t i = 0; i < res.size(); ++i) {
            const PortResult &r = res[i];
            if (!r.portable) continue;   // 不是指標碼的不列出來
            std::string cat;
            const std::string zh = session().translator().translate(r.name, &cat);
            const std::string conf = !r.ok ? "找不到" : r.votes >= 2 ? "信心高" : "信心低";
            auto *it = new tsl::elm::ListItem(zh, conf);
            if (r.ok) onA(it, [this, i, it]() { add(i, it); });
            list_->addItem(it);
            items_.push_back(it);
        }
        if (items_.empty() && !res.empty()) {
            auto *it = new tsl::elm::ListItem("其他版本沒有可以移植的指標碼");
            list_->addItem(it);
            items_.push_back(it);
        }
    }

    void add(size_t i, tsl::elm::ListItem *it) {
        const PortResult &r = searchTask().portResults()[i];
        Cheat c = r.cheat;
        c.name = "[移植] " + r.name;
        std::string m;
        const Result rc = session().addOwnCheat(c, false, m);
        setMsg(m, R_SUCCEEDED(rc) ? ui::Tone::Ok : ui::Tone::Error);
        if (R_SUCCEEDED(rc)) it->setValue("已加入", true);
    }

    void addAll() {
        int n = 0;
        const auto &res = searchTask().portResults();
        for (size_t i = 0; i < res.size(); ++i) {
            if (!res[i].ok || res[i].votes < 2) continue;
            Cheat c = res[i].cheat;
            c.name = "[移植] " + res[i].name;
            std::string m;
            if (R_SUCCEEDED(session().addOwnCheat(c, false, m))) ++n;
        }
        setMsg(n ? "已加入 " + std::to_string(n) + " 條（關閉中），請到首頁開啟測試" : "沒有信心高的結果，請先按「開始移植」",
               n ? ui::Tone::Ok : ui::Tone::Warn);
    }

    std::string help() {
        auto *f = getFocusedElement();
        if (f == start_) return "開始移植：下載同一款遊戲其他版本的碼，在你的版本裡找出每條碼的新起點。遊戲更新後通常只有起點換位置，後面的偏移不變。搜尋時遊戲會暫停。";
        if (f == all_) return "把信心高的碼一次加入（預設關閉）。信心高 = 有兩條以上的碼都指向同樣的位移，通常是對的。";
        for (auto *e : items_)
            if (f == e) return "信心高：多條碼互相印證，通常可以直接用。信心低：只有這條碼自己的推測，要開啟後在遊戲裡確認。按 A 加入（預設關閉），到首頁開啟測試；不對就關掉。";
        return "只能移植「指標碼」（以 580F 開頭的那種）；直接寫固定位址、或改程式碼（ASM）的碼沒辦法自動移植。";
    }

    tsl::elm::List *list_ = nullptr;
    tsl::elm::ListItem *start_ = nullptr, *all_ = nullptr;
    std::vector<tsl::elm::Element *> items_;
    std::string msg_;
    ui::Tone tone_ = ui::Tone::Ok;
};

// ═══════════════ 線上碼庫：下載、選組合 ═══════════════
class GuiOnline : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto &s = session();
        auto frame = new tsl::elm::OverlayFrame("線上碼庫", "BID " + s.bid());
        list_ = new tsl::elm::List();

        list_->addItem(new ui::StatusBar([this](std::string &t, ui::Tone &tone) {
            if (downloader().running()) { t = downloader().message(); tone = ui::Tone::Warn; }
            else { t = msg_; tone = tone_; }
        }));

        auto update = new tsl::elm::ListItem("從線上更新", "下載最新");
        onA(update, [this]() { startDownload(); });
        list_->addItem(update);

        auto port = new tsl::elm::ListItem("從其他版本移植", "沒碼時用");
        onA(port, []() { tsl::changeTo<GuiPort>(); });
        list_->addItem(port);

        buildGroups();
        // 本機還沒有這個版本的碼：一進來就自動下載
        if (groups_.empty()) startDownload();
        else setMsg("選一個組合載入；主碼不同的碼不能混用", ui::Tone::Ok);

        frame->setContent(list_);
        return frame;
    }

    void update() override {
        if (!downloader().takeFinished()) return;
        const auto st = downloader().state();
        setMsg(downloader().message(), st == Downloader::State::Done ? ui::Tone::Ok :
                                       st == Downloader::State::NotFound ? ui::Tone::Warn : ui::Tone::Error);
        buildGroups();
    }

private:
    void startDownload() {
        if (downloader().running()) return;
        downloader().start(session().tid(), session().bid());
    }

    void setMsg(const std::string &m, ui::Tone t) { msg_ = m; tone_ = t; }

    // 組合清單：下載完會重建一次
    void buildGroups() {
        auto *focused = getFocusedElement();
        for (auto *e : groupElems_) if (e == focused) { removeFocus(); break; }
        for (auto *e : groupElems_) list_->removeItem(e);
        groupElems_.clear();
        items_.clear();

        groups_ = session().dbGroups();
        for (size_t i = 0; i < groups_.size(); ++i) {
            const DbGroup g = groups_[i];
            std::string src = g.sources.size() > 30 ? g.sources.substr(0, 30) + "…" : g.sources;
            auto header = new tsl::elm::CategoryHeader("組合 " + std::to_string(i + 1) + "：" + src, true);
            auto item = new tsl::elm::ListItem(i == 0 ? "載入（推薦）" : "載入", label(g));
            onA(item, [this, g]() {
                std::string m;
                const Result rc = session().loadGroup(g, m);
                setMsg(m, R_SUCCEEDED(rc) ? ui::Tone::Ok : ui::Tone::Error);
                for (size_t k = 0; k < items_.size(); ++k) items_[k]->setValue(label(groups_[k]));
            });
            list_->addItem(header);
            list_->addItem(item);
            groupElems_.push_back(header);
            groupElems_.push_back(item);
            items_.push_back(item);
        }
    }

    static std::string label(const DbGroup &g) {
        if (session().loadedGroup() == g.file) return "使用中";
        std::string v = std::to_string(g.count) + " 條";
        if (g.needsPatch) v += "・修補";
        return v;
    }

    tsl::elm::List *list_ = nullptr;
    std::vector<DbGroup> groups_;
    std::vector<tsl::elm::Element *> groupElems_;
    std::vector<tsl::elm::ListItem *> items_;
    std::string msg_;
    ui::Tone tone_ = ui::Tone::Ok;
};

// ═══════════════ 首頁（照示意圖：狀態條 → 分類膠囊 → 金手指列 → 更多功能） ═══════════════
class GuiMain : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto &s = session();
        std::string sub = s.hasGame() ? ("TID " + s.tid() + " · BID " + s.bid()) : "繁中金手指";
        const std::string title = s.gameTitle().empty() ? "金手指" : s.gameTitle();
        auto frame = new tsl::elm::OverlayFrame(title, sub);
        list_ = new tsl::elm::List();

        list_->addItem(new ui::StatusBar(mainStatus));

        if (s.hasGame()) {
            buildGameSection();
        } else {
            // 連不上或沒遊戲：給一個重新連線，成功後畫面會自動補上分類和金手指
            retry_ = new tsl::elm::ListItem("重新連線");
            onA(retry_, []() { session().close(); session().open(); });
            list_->addItem(retry_);
        }

        structureGen_ = s.structureGeneration();
        frame->setContent(list_);
        return frame;
    }

    void update() override {
        auto &s = session();
        if (!chips_ && s.hasGame()) {          // 重新連線成功
            if (retry_) { removeFocus(); list_->removeItem(retry_); retry_ = nullptr; }
            buildGameSection();
            structureGen_ = s.structureGeneration();
            return;
        }
        // 碼的清單換了（載入新組合、搜尋產生新碼）才重建列；開關只是重畫，不用重建
        if (chips_ && s.structureGeneration() != structureGen_) {
            structureGen_ = s.structureGeneration();
            rebuildRows();
        }
    }

private:
    // 全部用附加的方式加入（List 是先插入再刪除，用附加才不會跟「重新連線」項目搶位置）
    void buildGameSection() {
        std::vector<std::string> cats(kCategories, kCategories + kCategoryCount);
        chips_ = new ui::CategoryChips(cats,
            [](const std::string &c) { return session().countInCategory(c); },
            [this](const std::string &c) { category_ = c; rebuildRows(); });
        list_->addItem(chips_);
        addRows(false);

        list_->addItem(new tsl::elm::CategoryHeader("更多"));
        auto off = new tsl::elm::ListItem("全部關閉");
        onA(off, []() { session().disableAll(); });
        list_->addItem(off);

        auto online = new tsl::elm::ListItem("線上碼庫", "切換組合");
        onA(online, []() { tsl::changeTo<GuiOnline>(); });
        list_->addItem(online);

        auto adv = new tsl::elm::ListItem("進階搜尋", "記憶體數值");
        onA(adv, []() { tsl::changeTo<GuiSearch>(); });
        list_->addItem(adv);
    }

    static ui::CheatRow::State query(u32 id) {
        for (const auto &c : session().cheats())
            if (c.id == id) return {true, c.enabled, c.isMaster, c.zhName, c.name};
        return {false, false, false, "", ""};
    }

    // 第一次建立用附加；之後重建要插在「狀態條、分類膠囊」後面（索引 2）
    void addRows(bool insert) {
        ssize_t index = insert ? kFirstRowIndex : -1;
        for (const auto &c : session().cheats()) {
            if (category_ != "全部" && c.category != category_) continue;
            auto row = new ui::CheatRow(c.id, query, [](u32 id, bool on) { session().toggle(id, on); });
            rows_.push_back(row);
            list_->addItem(row, 0, index);
            if (insert) ++index;
        }
    }

    void rebuildRows() {
        // 焦點在要刪掉的列上就先移開，避免指到已刪除的元件
        auto *focused = getFocusedElement();
        for (auto *r : rows_) if (r == focused) { removeFocus(); break; }
        for (auto *r : rows_) list_->removeItem(r);
        rows_.clear();
        addRows(true);
    }

    tsl::elm::List *list_ = nullptr;
    ui::CategoryChips *chips_ = nullptr;
    std::vector<ui::CheatRow *> rows_;
    tsl::elm::ListItem *retry_ = nullptr;
    std::string category_ = "全部";
    static constexpr ssize_t kFirstRowIndex = 2;
    u32 structureGen_ = 0;
};

class CheatOverlay : public tsl::Overlay {
public:
    // libtesla 平常不會掛載 SD 卡（只在它自己讀設定時短暫掛載），
    // 所以我們要自己在啟動時掛載、結束時卸載；沒掛載時呼叫 mkdir 會直接當機
    void initServices() override { fsdevMountSdmc(); }
    void exitServices() override {
        if (searchTask().running()) searchTask().cancel();
        searchTask().join();
        downloader().join();
        session().setKeepAlive(false);
        session().close();
        fsdevUnmountDevice("sdmc");
    }

    // 顯示時才佔用 dmnt session，隱藏時馬上放掉，避免跟 Breeze／EdiZon-SE 搶
    // （背景搜尋中例外，搜完會自動放掉）
    void onShow() override { session().open(); }
    void onHide() override { session().close(); }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        session().open();
        return initially<GuiMain>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<CheatOverlay>(argc, argv);
}
