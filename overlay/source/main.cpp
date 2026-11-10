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

using namespace cheattw;

static const char *kCategories[] = { "全部", "戰鬥", "金錢", "道具", "移動", "畫面", "其他" };
constexpr int kCategoryCount = sizeof(kCategories) / sizeof(kCategories[0]);

static std::string statusLine() {
    auto &s = session();
    if (!s.isOpen()) return "dmnt 無法連線（請先關掉 Breeze／EdiZon）";
    if (!s.hasGame()) return "目前沒有執行中的遊戲";
    return "已開啟 " + std::to_string(s.enabledCount()) + " / " + std::to_string(s.cheats().size()) + " 條";
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

// ═══════════════ 金手指列表 ═══════════════
class GuiCheatList : public tsl::Gui {
public:
    explicit GuiCheatList(std::string category) : category_(std::move(category)) {}

    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("金手指 · " + category_, statusLine());
        auto list = new tsl::elm::List();

        bool any = false;
        for (const auto &c : session().cheats()) {
            if (category_ != "全部" && c.category != category_) continue;
            any = true;
            if (c.isMaster) {
                list->addItem(new tsl::elm::ListItem(c.zhName, "主碼・自動"));
                continue;
            }
            auto item = new tsl::elm::ToggleListItem(c.zhName, c.enabled, "開啟", "關閉");
            const u32 id = c.id;
            item->setStateChangedListener([id, item](bool on) {
                if (R_FAILED(session().toggle(id, on))) item->setState(!on);
            });
            list->addItem(item);
        }
        if (!any) list->addItem(new tsl::elm::ListItem("這個分類沒有金手指"));
        frame->setContent(list);
        return frame;
    }

private:
    std::string category_;
};

// ═══════════════ 線上碼庫：選組合 ═══════════════
class GuiOnline : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto &s = session();
        auto frame = new tsl::elm::OverlayFrame("線上碼庫", "BID " + s.bid().substr(0, 8));
        auto list = new tsl::elm::List();

        const auto groups = s.dbGroups();
        if (groups.empty()) {
            list->addItem(new tsl::elm::CategoryHeader("這個版本還沒有碼庫快取"));
            list->addItem(new tsl::elm::ListItem("請用主程式下載，或把檔案放到"));
            list->addItem(new tsl::elm::ListItem("switch/CheatTW/db/" + s.tid().substr(0, 8) + "…"));
        } else {
            list->addItem(new tsl::elm::CategoryHeader("主碼不同的碼不能混用，所以分成組合"));
            for (size_t i = 0; i < groups.size(); ++i) {
                const DbGroup g = groups[i];
                std::string src = g.sources.size() > 36 ? g.sources.substr(0, 36) + "…" : g.sources;
                list->addItem(new tsl::elm::CategoryHeader("組合 " + std::to_string(i + 1) + "：" + src, true));
                auto item = new tsl::elm::ListItem(i == 0 ? "載入（推薦）" : "載入", label(g));
                onA(item, [g, item]() {
                    std::string msg;
                    session().loadGroup(g, msg);
                    item->setValue(msg);
                });
                list->addItem(item);
            }
        }
        frame->setContent(list);
        return frame;
    }

private:
    static std::string label(const DbGroup &g) {
        std::string v = std::to_string(g.count) + " 條";
        if (g.needsPatch) v += "・要修補檔";
        if (session().loadedGroup() == g.file) v += "・使用中";
        return v;
    }
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
        msg_ = new tsl::elm::ListItem("", "");

        if (known) {
            auto lockNow = new tsl::elm::ListItem("鎖定為目前數值");
            onA(lockNow, [this, t, region, offset, where]() { makeLock(t, region, offset, where, current_); });
            list->addItem(lockNow);

            auto lockCustom = new tsl::elm::ListItem("鎖定為自訂數值…");
            onA(lockCustom, [this, t, region, offset, where]() {
                tsl::changeTo<GuiValueEdit>("鎖定數值", formatValue(t, current_), isFloat(t),
                    [this, t, region, offset, where](const std::string &s) {
                        u64 bits;
                        if (parseValue(t, s, bits)) makeLock(t, region, offset, where, bits);
                        else msg_->setValue("數值格式不對");
                    });
            });
            list->addItem(lockCustom);
        }

        auto writeOnce = new tsl::elm::ListItem("只改一次（不鎖定）…");
        onA(writeOnce, [this, t]() {
            tsl::changeTo<GuiValueEdit>("改成", formatValue(t, current_), isFloat(t),
                [this, t](const std::string &s) {
                    u64 bits;
                    if (!parseValue(t, s, bits)) { msg_->setValue("數值格式不對"); return; }
                    Result rc = dmntchtWriteCheatProcessMemory(addr_, &bits, valueSize(t));
                    msg_->setValue(R_SUCCEEDED(rc) ? "已寫入" : "寫入失敗");
                });
        });
        list->addItem(writeOnce);

        list->addItem(msg_);
        list->addItem(new tsl::elm::CategoryHeader("位址每次開遊戲可能不同，要跨次使用請用 Breeze 指標搜尋", true));
        frame->setContent(list);
        return frame;
    }

private:
    static bool isFloat(ValueType t) { return t == ValueType::F32 || t == ValueType::F64; }

    void makeLock(ValueType t, u8 region, u64 offset, const std::string &where, u64 bits) {
        Cheat c = makeStaticWriteCheat("[自製] 鎖定 " + where + " = " + formatValue(t, bits), region, offset, t, bits);
        std::string m;
        session().addOwnCheat(c, true, m);
        msg_->setValue(m);
    }

    u64 addr_, current_;
    tsl::elm::ListItem *msg_ = nullptr;
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
        if (eng.count() > kShow) list->addItem(new tsl::elm::CategoryHeader("結果太多，請在遊戲裡讓數值變化後再搜一次", true));
        frame->setContent(list);
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

        status_ = new tsl::elm::ListItem("狀態", "");
        list->addItem(status_);

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

        auto resetItem = new tsl::elm::ListItem("重新開始");
        onA(resetItem, [this]() {
            searchTask().reset();
            g_ui.cond = Cond::Equal;
            status_->setValue("已清除");
            refreshLabels();
        });
        list->addItem(resetItem);

        list->addItem(new tsl::elm::CategoryHeader("搜尋時遊戲會暫停，搜完自動繼續", true));

        refreshLabels();
        frame->setContent(list);
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
            status_->setValue("搜尋中 " + std::to_string(pct) + "%");
        }
        if (task.takeFinished()) {
            status_->setValue(std::string(searchResultText(task.lastResult())));
            refreshLabels();
        }
    }

private:
    void onAction() {
        auto &task = searchTask();
        if (task.running()) { task.cancel(); return; }
        u64 bits = 0;
        if (condNeedsValue(g_ui.cond) && !parseValue(g_ui.type, g_ui.value, bits)) {
            status_->setValue("數值格式不對");
            return;
        }
        const bool first = !task.engine().started();
        if (!task.start(first, g_ui.type, g_ui.cond, bits, g_ui.scope)) {
            status_->setValue("無法開始（沒有遊戲？）");
            return;
        }
        status_->setValue("搜尋中 0%");
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
        if (!statusShown_) {
            status_->setValue(task.running() ? "搜尋中…" : (started ? "可以接著搜尋" : "先輸入目前看到的數值"));
            statusShown_ = true;
        }
    }

    bool statusShown_ = false;

    tsl::elm::ListItem *status_ = nullptr, *type_ = nullptr, *cond_ = nullptr, *value_ = nullptr,
                       *scope_ = nullptr, *action_ = nullptr, *results_ = nullptr;
    u32 tick_ = 0;
};

// ═══════════════ 首頁 ═══════════════
class GuiMain : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto &s = session();
        std::string sub = s.hasGame() ? ("TID " + s.tid() + " · BID " + s.bid().substr(0, 8)) : "繁中金手指";
        auto frame = new tsl::elm::OverlayFrame("金手指", sub);
        auto list = new tsl::elm::List();

        status_ = new tsl::elm::ListItem("狀態", statusLine());
        list->addItem(status_);

        if (s.hasGame()) {
            list->addItem(new tsl::elm::CategoryHeader("分類"));
            for (int i = 0; i < kCategoryCount; ++i) {
                const std::string name = kCategories[i];
                cats_[i] = new tsl::elm::ListItem(name, "");
                onA(cats_[i], [name]() { tsl::changeTo<GuiCheatList>(name); });
                list->addItem(cats_[i]);
            }

            auto off = new tsl::elm::ListItem("全部關閉");
            onA(off, []() { session().disableAll(); });
            list->addItem(off);

            list->addItem(new tsl::elm::CategoryHeader("找不到想要的碼？"));
            auto online = new tsl::elm::ListItem("線上碼庫", "切換組合");
            onA(online, []() { tsl::changeTo<GuiOnline>(); });
            list->addItem(online);

            auto adv = new tsl::elm::ListItem("進階搜尋", "記憶體數值");
            onA(adv, []() { tsl::changeTo<GuiSearch>(); });
            list->addItem(adv);
        }

        lastGen_ = 0;
        update();
        frame->setContent(list);
        return frame;
    }

    void update() override {
        auto &s = session();
        if (s.generation() == lastGen_) return;   // 沒變就不動，避免每幀重算
        lastGen_ = s.generation();
        status_->setValue(statusLine());
        if (!s.hasGame()) return;
        for (int i = 0; i < kCategoryCount; ++i)
            if (cats_[i]) {
                const size_t n = s.countInCategory(kCategories[i]);
                cats_[i]->setValue(std::to_string(n) + " 條", n == 0);
            }
    }

private:
    tsl::elm::ListItem *status_ = nullptr;
    tsl::elm::ListItem *cats_[kCategoryCount] = {};
    u32 lastGen_ = 0;
};

class CheatOverlay : public tsl::Overlay {
public:
    void initServices() override {}
    void exitServices() override {
        if (searchTask().running()) searchTask().cancel();
        session().setKeepAlive(false);
        session().close();
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
