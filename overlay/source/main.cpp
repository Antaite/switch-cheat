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

// ═══════════════ 數值編輯器（Tesla 沒有鍵盤，用十字鍵改數字） ═══════════════
// ↑↓ 改這一位　←→ 換位置　Y 加一位　X 刪一位　－ 正負號　A 確定　B 取消
class GuiValueEdit : public tsl::Gui {
public:
    GuiValueEdit(std::string title, std::string initial, bool allowFloat,
                 std::function<void(const std::string &)> done)
        : title_(std::move(title)), text_(initial.empty() ? "0" : std::move(initial)),
          allowFloat_(allowFloat), done_(std::move(done)) {
        cursor_ = text_.size() - 1;
    }

    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame(title_, "↑↓ 改數字　←→ 移動　Y 加位　X 刪位");
        auto list = new tsl::elm::List();
        display_ = new tsl::elm::ListItem(render());
        list->addItem(display_);
        list->addItem(new tsl::elm::ListItem(allowFloat_ ? "小數點：在數字上按 ↑↓ 可以換成 ." : "整數"));
        list->addItem(new tsl::elm::ListItem("－ 鍵：切換正負號"));
        list->addItem(new tsl::elm::ListItem("A 確定　B 取消"));
        frame->setContent(list);
        return frame;
    }

    bool handleInput(u64 down, u64, const HidTouchState &, HidAnalogStickState, HidAnalogStickState) override {
        if (down & HidNpadButton_AnyUp)        { step(+1); }
        else if (down & HidNpadButton_AnyDown) { step(-1); }
        else if (down & HidNpadButton_AnyLeft) { if (cursor_ > 0) --cursor_; }
        else if (down & HidNpadButton_AnyRight){ if (cursor_ + 1 < text_.size()) ++cursor_; }
        else if (down & HidNpadButton_Y)       { if (text_.size() < 20) { text_.insert(cursor_ + 1, "0"); ++cursor_; } }
        else if (down & HidNpadButton_X)       { if (text_.size() > 1) { text_.erase(cursor_, 1); if (cursor_ >= text_.size()) cursor_ = text_.size() - 1; } }
        else if (down & HidNpadButton_Minus)   { if (text_[0] == '-') { text_.erase(0, 1); if (cursor_) --cursor_; } else { text_.insert(0, "-"); ++cursor_; } }
        else if (down & HidNpadButton_A)       { auto cb = done_; std::string t = text_; tsl::goBack(); cb(t); return true; }
        else if (down & HidNpadButton_B)       { tsl::goBack(); return true; }
        else return false;
        display_->setText(render());
        return true;
    }

private:
    void step(int d) {
        char &ch = text_[cursor_];
        if (ch == '-') return;
        static const char digitsF[] = "0123456789.";
        const char *set = allowFloat_ ? digitsF : "0123456789";
        const int n = (int)std::strlen(set);
        const char *p = std::strchr(set, ch);
        int i = p ? (int)(p - set) : 0;
        char next = set[(i + d + n) % n];
        if (next == '.' && text_.find('.') != std::string::npos) next = set[(i + 2 * d + n) % n];  // 只能有一個小數點
        ch = next;
    }
    std::string render() const {
        std::string out;
        for (size_t i = 0; i < text_.size(); ++i) {
            if (i == cursor_) { out += "["; out += text_[i]; out += "]"; }
            else out += text_[i];
        }
        return out;
    }

    std::string title_, text_;
    size_t cursor_ = 0;
    bool allowFloat_;
    std::function<void(const std::string &)> done_;
    tsl::elm::ListItem *display_ = nullptr;
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

// ═══════════════ 進階搜尋：單一結果的操作 ═══════════════
class GuiResultAction : public tsl::Gui {
public:
    GuiResultAction(u64 addr, u64 current) : addr_(addr), current_(current) {}

    tsl::elm::Element *createUI() override {
        auto &task = searchTask();
        const ValueType t = task.engine().type();
        std::string where;
        u8 region = 1;
        u64 offset = 0;
        const bool known = task.locate(addr_, region, offset, where);

        auto frame = new tsl::elm::OverlayFrame(known ? where : "未知區段", "目前數值 " + formatValue(t, current_));
        auto list = new tsl::elm::List();
        list->addItem(new ui::StatusBar([this](std::string &txt, ui::Tone &tone) {
            txt = msg_.empty() ? "選擇要怎麼處理這個位址" : msg_;
            tone = msgTone_;
        }));

        if (known) {
            auto lockNow = lockNow_ = new tsl::elm::ListItem("鎖定為目前數值");
            onA(lockNow, [this, t, region, offset, where]() { makeLock(t, region, offset, where, current_); });
            list->addItem(lockNow);

            auto lockCustom = lockCustom_ = new tsl::elm::ListItem("鎖定為自訂數值…");
            onA(lockCustom, [this, t, region, offset, where]() {
                tsl::changeTo<GuiValueEdit>("鎖定數值", formatValue(t, current_), isFloat(t),
                    [this, t, region, offset, where](const std::string &s) {
                        u64 bits;
                        if (parseValue(t, s, bits)) makeLock(t, region, offset, where, bits);
                        else setMsg("數值格式不對", false);
                    });
            });
            list->addItem(lockCustom);
        }

        auto writeOnce = writeOnce_ = new tsl::elm::ListItem("只改一次（不鎖定）…");
        onA(writeOnce, [this, t]() {
            tsl::changeTo<GuiValueEdit>("改成", formatValue(t, current_), isFloat(t),
                [this, t](const std::string &s) {
                    u64 bits;
                    if (!parseValue(t, s, bits)) { setMsg("數值格式不對", false); return; }
                    Result rc = dmntchtWriteCheatProcessMemory(addr_, &bits, valueSize(t));
                    setMsg(R_SUCCEEDED(rc) ? "已寫入" : "寫入失敗", R_SUCCEEDED(rc));
                });
        });
        list->addItem(writeOnce);

        frame->setContent(new ui::SplitView(list, new ui::HelpBox([this]() {
            auto *f = getFocusedElement();
            std::string h;
            if (f == lockNow_) h = "鎖定為目前數值：產生一條金手指，讓這個位址一直維持現在的值（例如血量不會再減少）。";
            else if (f == lockCustom_) h = "鎖定為自訂數值：先輸入想要的數字（例如金錢 999999），再產生金手指一直維持這個值。";
            else if (f == writeOnce_) h = "只改一次：直接改成你輸入的值，不產生金手指；遊戲之後還是會照常改變它。";
            else h = "選擇要怎麼處理這個位址。";
            return h + "\n產生的金手指名稱開頭是 [自製]，會存進大氣層的金手指檔。位址每次開遊戲可能不同，要跨次使用請用 Breeze 的指標搜尋。";
        }), 200));
        return frame;
    }

private:
    static bool isFloat(ValueType t) { return t == ValueType::F32 || t == ValueType::F64; }

    void makeLock(ValueType t, u8 region, u64 offset, const std::string &where, u64 bits) {
        Cheat c = makeStaticWriteCheat("[自製] 鎖定 " + where + " = " + formatValue(t, bits), region, offset, t, bits);
        std::string m;
        const Result rc = session().addOwnCheat(c, true, m);
        setMsg(m, R_SUCCEEDED(rc));
    }

    void setMsg(const std::string &m, bool ok) { msg_ = m; msgTone_ = ok ? ui::Tone::Ok : ui::Tone::Error; }

    u64 addr_, current_;
    tsl::elm::ListItem *lockNow_ = nullptr, *lockCustom_ = nullptr, *writeOnce_ = nullptr;
    std::string msg_;
    ui::Tone msgTone_ = ui::Tone::Ok;
};

// ═══════════════ 進階搜尋：結果列表 ═══════════════
class GuiResults : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto &task = searchTask();
        auto &eng = task.engine();
        auto frame = new tsl::elm::OverlayFrame("搜尋結果", "共 " + std::to_string(eng.count()) + " 筆，顯示前 " +
                                                std::to_string(std::min<u64>(eng.count(), kShow)) + " 筆");
        auto list = new tsl::elm::List();

        // 只讀前 40 筆（640 bytes），結果再多也不會多吃記憶體
        static Candidate buf[kShow];
        const size_t n = eng.peek(buf, kShow, 0);
        const size_t w = valueSize(eng.type());
        for (size_t i = 0; i < n; ++i) {
            u64 cur = 0;
            const bool ok = R_SUCCEEDED(dmntchtReadCheatProcessMemory(buf[i].addr, &cur, w));
            std::string where;
            u8 r; u64 off;
            if (!task.locate(buf[i].addr, r, off, where)) {
                char tmp[24];
                std::snprintf(tmp, sizeof(tmp), "0x%010llX", (unsigned long long)buf[i].addr);
                where = tmp;
            }
            auto item = new tsl::elm::ListItem(where, ok ? formatValue(eng.type(), cur) : "讀不到");
            const u64 addr = buf[i].addr;
            onA(item, [addr, cur]() { tsl::changeTo<GuiResultAction>(addr, cur); });
            list->addItem(item);
        }
        if (n == 0) list->addItem(new tsl::elm::ListItem("沒有結果"));
        const bool tooMany = eng.count() > kShow;
        frame->setContent(new ui::SplitView(list, new ui::HelpBox([tooMany]() {
            return std::string("每一列是一個位址和它現在的值。H+ 開頭在 Heap，M+ 開頭在主程式。按 A 選擇要鎖定或修改。") +
                   (tooMany ? "\n結果還很多，建議回上一頁，在遊戲裡讓數值變化後再搜一次。" : "");
        }), 170));
        return frame;
    }

private:
    static constexpr size_t kShow = 40;
};

// ═══════════════ 進階搜尋：主畫面 ═══════════════
struct SearchUiState {
    ValueType type = ValueType::U32;
    Cond cond = Cond::Equal;
    std::string value = "100";
    SearchScope scope = SearchScope::Heap;
};
static SearchUiState g_ui;

class GuiSearch : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("進階搜尋", "記憶體數值搜尋");
        auto list = new tsl::elm::List();

        list->addItem(new ui::StatusBar([this](std::string &t, ui::Tone &tone) { t = statusText_; tone = statusTone_; }));

        list->addItem(new tsl::elm::CategoryHeader("條件"));
        type_ = new tsl::elm::ListItem("資料型別", "");
        onA(type_, [this]() {
            if (searchTask().engine().started() || searchTask().running()) return;   // 第一次搜尋後型別就固定了
            g_ui.type = (ValueType)(((int)g_ui.type + 1) % 6);
            refreshLabels();
        });
        list->addItem(type_);

        cond_ = new tsl::elm::ListItem("比較方式", "");
        onA(cond_, [this]() {
            const int maxCond = searchTask().engine().started() ? 7 : 3;   // 第一次只能 等於／大於／小於
            g_ui.cond = (Cond)(((int)g_ui.cond + 1) % maxCond);
            refreshLabels();
        });
        list->addItem(cond_);

        value_ = new tsl::elm::ListItem("數值", "");
        onA(value_, [this]() {
            if (!condNeedsValue(g_ui.cond)) return;
            const bool f = g_ui.type == ValueType::F32 || g_ui.type == ValueType::F64;
            tsl::changeTo<GuiValueEdit>("搜尋數值", g_ui.value, f, [this](const std::string &s) {
                g_ui.value = s;
                refreshLabels();
            });
        });
        list->addItem(value_);

        scope_ = new tsl::elm::ListItem("範圍", "");
        onA(scope_, [this]() {
            if (searchTask().engine().started()) return;
            g_ui.scope = g_ui.scope == SearchScope::Heap ? SearchScope::MainAndHeap : SearchScope::Heap;
            refreshLabels();
        });
        list->addItem(scope_);

        list->addItem(new tsl::elm::CategoryHeader("動作"));
        action_ = new tsl::elm::ListItem("", "");
        onA(action_, [this]() { onAction(); });
        list->addItem(action_);

        results_ = new tsl::elm::ListItem("查看結果", "");
        onA(results_, []() {
            if (searchTask().engine().started() && !searchTask().running()) tsl::changeTo<GuiResults>();
        });
        list->addItem(results_);

        resetItem_ = new tsl::elm::ListItem("重新開始");
        onA(resetItem_, [this]() {
            searchTask().reset();
            g_ui.cond = Cond::Equal;
            setStatus("已清除");
            refreshLabels();
        });
        list->addItem(resetItem_);

        refreshLabels();
        frame->setContent(new ui::SplitView(list, new ui::HelpBox([this]() { return helpText(); }), 200));
        return frame;
    }

    void update() override {
        auto &task = searchTask();
        if (task.running()) {
            // 每 10 幀更新一次百分比就夠了
            if (++tick_ % 10) return;
            const u64 total = task.progress().total;
            const u64 done = task.progress().done;
            const int pct = total ? (int)(done * 100 / total) : 0;
            setStatus("搜尋中 " + std::to_string(pct) + "%（可以按「取消搜尋」）", ui::Tone::Warn);
        }
        if (task.takeFinished()) {
            const SearchResult r = task.lastResult();
            setStatus(r == SearchResult::Ok ? "完成，找到 " + std::to_string(task.engine().count()) + " 筆"
                                            : std::string(searchResultText(r)),
                      r == SearchResult::Ok ? ui::Tone::Ok : ui::Tone::Error);
            refreshLabels();
        }
    }

private:
    void onAction() {
        auto &task = searchTask();
        if (task.running()) { task.cancel(); return; }
        u64 bits = 0;
        if (condNeedsValue(g_ui.cond) && !parseValue(g_ui.type, g_ui.value, bits)) {
            setStatus("數值格式不對", ui::Tone::Error);
            return;
        }
        const bool first = !task.engine().started();
        if (!task.start(first, g_ui.type, g_ui.cond, bits, g_ui.scope)) {
            setStatus("無法開始（沒有遊戲？）", ui::Tone::Error);
            return;
        }
        setStatus("搜尋中 0%", ui::Tone::Warn);
        refreshLabels();
    }

    void refreshLabels() {
        auto &task = searchTask();
        const bool started = task.engine().started();
        if (!started && condNeedsPrevious(g_ui.cond)) g_ui.cond = Cond::Equal;
        type_->setValue(valueTypeName(g_ui.type), started);
        cond_->setValue(condName(g_ui.cond));
        value_->setValue(condNeedsValue(g_ui.cond) ? g_ui.value : "（這個方式不用數值）", !condNeedsValue(g_ui.cond));
        scope_->setValue(g_ui.scope == SearchScope::Heap ? "Heap（一般建議）" : "主程式 + Heap", started);
        action_->setText(task.running() ? "取消搜尋" : (started ? "接著搜尋" : "開始搜尋"));
        results_->setValue(started ? std::to_string(task.engine().count()) + " 筆" : "—");
        if (statusText_.empty())
            setStatus(task.running() ? "搜尋中…" : (started ? "在遊戲裡讓數值變化後，再接著搜尋" : "先輸入遊戲裡目前看到的數值"));
    }

    void setStatus(const std::string &t, ui::Tone tone = ui::Tone::Ok) { statusText_ = t; statusTone_ = tone; }

    // 說明框內容：依游標所在的選項，說明用途和目前選的值代表什麼
    std::string helpText() {
        auto *f = getFocusedElement();
        const bool started = searchTask().engine().started();
        if (f == type_) {
            static const char *desc[] = {
                "整數 1 byte（0～255）：等級、道具數量這類小數字。",
                "整數 2 bytes（0～65535）：中等大小的數字。",
                "整數 4 bytes：最常用，血量、金錢、經驗值大多是這種。不確定就選它。",
                "整數 8 bytes：非常大的數字，少見。",
                "小數 float：有小數點的值，例如移動速度、計時器、血條百分比。",
                "小數 double：精度更高的小數，少見。",
            };
            return std::string("資料型別：數值在記憶體裡的存法，按 A 切換。\n目前：") + desc[(int)g_ui.type] +
                   (started ? "\n第一次搜尋後就不能改，要換請按「重新開始」。" : "");
        }
        if (f == cond_) {
            static const char *desc[] = {
                "等於：找跟輸入數值完全一樣的位址。第一次搜尋最常用。",
                "大於：找比輸入數值大的位址，不知道確切數字時用。",
                "小於：找比輸入數值小的位址。",
                "變大了：跟上次搜尋時比，數值變大了（例如撿到錢之後）。不用輸入數值。",
                "變小了：跟上次比變小了（例如被打扣血之後）。不用輸入數值。",
                "有變：跟上次比有任何變化。",
                "沒變：跟上次比完全沒變，用來排除會亂跳的位址。",
            };
            return std::string("比較方式：按 A 切換。\n目前：") + desc[(int)g_ui.cond] +
                   (started ? "" : "\n第一次只能用等於／大於／小於，搜過一次後才有「變大了」這些。");
        }
        if (f == value_)
            return condNeedsValue(g_ui.cond)
                ? "數值：輸入遊戲畫面上看到的數字。按 A 編輯：↑↓ 改數字、←→ 換位置、Y 加一位、X 刪一位。"
                : "數值：目前的比較方式是跟上次比，不用輸入數值。";
        if (f == scope_)
            return g_ui.scope == SearchScope::Heap
                ? "範圍：按 A 切換。\n目前：Heap，遊戲執行時的資料區，血量、金錢幾乎都在這裡，搜尋比較快。"
                : "範圍：按 A 切換。\n目前：主程式＋Heap，連主程式的固定資料也搜，比較慢。Heap 找不到時再用。";
        if (f == action_)
            return searchTask().running()
                ? "取消搜尋：停止目前的搜尋，上一次的結果會保留。"
                : started ? "接著搜尋：在遊戲裡讓數值變化後按這裡，從上次的結果裡再篩一次。通常 2～4 次就剩幾筆。"
                          : "開始搜尋：搜尋時遊戲會暫停，搜完自動繼續。結果存在 SD 卡，不佔記憶體。";
        if (f == results_)
            return "查看結果：列出找到的位址（最多 40 筆）。剩幾筆時再看，選一筆可以鎖定或直接改數值。";
        if (f == resetItem_)
            return "重新開始：清掉目前的結果，換別的數值或型別時用。";
        return "用法：① 輸入目前數值 → 開始搜尋 ② 在遊戲裡讓數值改變 ③ 輸入新數值 → 接著搜尋 ④ 剩幾筆時查看結果並鎖定。";
    }

    std::string statusText_;
    ui::Tone statusTone_ = ui::Tone::Ok;

    tsl::elm::ListItem *resetItem_ = nullptr;
    tsl::elm::ListItem *type_ = nullptr, *cond_ = nullptr, *value_ = nullptr,
                       *scope_ = nullptr, *action_ = nullptr, *results_ = nullptr;
    u32 tick_ = 0;
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
