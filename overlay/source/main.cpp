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

// ═══════════════ 線上碼庫：選組合 ═══════════════
class GuiOnline : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto &s = session();
        auto frame = new tsl::elm::OverlayFrame("線上碼庫", "BID " + s.bid().substr(0, 8));
        auto list = new tsl::elm::List();

        groups_ = s.dbGroups();
        msg_ = groups_.empty() ? "這個版本還沒有碼庫快取" : "主碼不同的碼不能混用，所以分成組合";
        tone_ = groups_.empty() ? ui::Tone::Warn : ui::Tone::Ok;
        list->addItem(new ui::StatusBar([this](std::string &t, ui::Tone &tone) { t = msg_; tone = tone_; }));

        if (groups_.empty()) {
            list->addItem(new tsl::elm::CategoryHeader("請用主程式下載，或把檔案放到"));
            list->addItem(new tsl::elm::CategoryHeader("switch/CheatTW/db/遊戲TID/", true));
        }
        for (size_t i = 0; i < groups_.size(); ++i) {
            const DbGroup g = groups_[i];
            std::string src = g.sources.size() > 30 ? g.sources.substr(0, 30) + "…" : g.sources;
            list->addItem(new tsl::elm::CategoryHeader("組合 " + std::to_string(i + 1) + "：" + src, true));
            auto item = new tsl::elm::ListItem(i == 0 ? "載入（推薦）" : "載入", label(g));
            items_.push_back(item);
            onA(item, [this, g]() {
                std::string m;
                const Result rc = session().loadGroup(g, m);
                msg_ = m;
                tone_ = R_SUCCEEDED(rc) ? ui::Tone::Ok : ui::Tone::Error;
                for (size_t k = 0; k < items_.size(); ++k) items_[k]->setValue(label(groups_[k]));
            });
            list->addItem(item);
        }
        frame->setContent(list);
        return frame;
    }

private:
    static std::string label(const DbGroup &g) {
        if (session().loadedGroup() == g.file) return "使用中";
        std::string v = std::to_string(g.count) + " 條";
        if (g.needsPatch) v += "・修補";
        return v;
    }

    std::vector<DbGroup> groups_;
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
            auto lockNow = new tsl::elm::ListItem("鎖定為目前數值");
            onA(lockNow, [this, t, region, offset, where]() { makeLock(t, region, offset, where, current_); });
            list->addItem(lockNow);

            auto lockCustom = new tsl::elm::ListItem("鎖定為自訂數值…");
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

        auto writeOnce = new tsl::elm::ListItem("只改一次（不鎖定）…");
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

        list->addItem(new tsl::elm::CategoryHeader("位址每次開遊戲可能不同，要跨次使用請用 Breeze 指標搜尋", true));
        frame->setContent(list);
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

        auto resetItem = new tsl::elm::ListItem("重新開始");
        onA(resetItem, [this]() {
            searchTask().reset();
            g_ui.cond = Cond::Equal;
            setStatus("已清除");
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

    std::string statusText_;
    ui::Tone statusTone_ = ui::Tone::Ok;

    tsl::elm::ListItem *type_ = nullptr, *cond_ = nullptr, *value_ = nullptr,
                       *scope_ = nullptr, *action_ = nullptr, *results_ = nullptr;
    u32 tick_ = 0;
};

// ═══════════════ 首頁（照示意圖：狀態條 → 分類膠囊 → 金手指列 → 更多功能） ═══════════════
class GuiMain : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto &s = session();
        std::string sub = s.hasGame() ? ("TID " + s.tid() + " · BID " + s.bid().substr(0, 8)) : "繁中金手指";
        auto frame = new tsl::elm::OverlayFrame("金手指", sub);
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
