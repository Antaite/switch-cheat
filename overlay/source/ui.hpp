// 自訂畫面元件：照示意圖的風格（狀態條、分類膠囊、雙行金手指 + 開關膠囊）
#pragma once
#include <tesla.hpp>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ui {

using tsl::Color;
using tsl::gfx::Renderer;

// 顏色（每個通道 0x0–0xF）
constexpr Color kAccent     = {0x3, 0xD, 0xC, 0xF};   // 青綠 #3DD6C4
constexpr Color kAccentText = {0x0, 0x0, 0x0, 0xF};
constexpr Color kText       = {0xF, 0xF, 0xF, 0xF};
constexpr Color kSub        = {0x9, 0xA, 0xA, 0xF};
constexpr Color kPillOff    = {0x2, 0x3, 0x3, 0xF};
constexpr Color kPillOffTxt = {0xB, 0xB, 0xC, 0xF};
constexpr Color kLine       = {0x2, 0x2, 0x3, 0xF};
constexpr Color kOkBg       = {0x1, 0x3, 0x2, 0xF};
constexpr Color kOkText     = {0x8, 0xE, 0xB, 0xF};
constexpr Color kOkDot      = {0x4, 0xD, 0x8, 0xF};
constexpr Color kWarnBg     = {0x3, 0x2, 0x0, 0xF};
constexpr Color kWarnText   = {0xF, 0xC, 0x8, 0xF};
constexpr Color kWarnDot    = {0xF, 0x9, 0x4, 0xF};
constexpr Color kErrBg      = {0x3, 0x1, 0x1, 0xF};
constexpr Color kErrText    = {0xF, 0xA, 0xA, 0xF};
constexpr Color kErrDot     = {0xF, 0x4, 0x4, 0xF};

inline Color a(Color c) { return Renderer::a(c); }

inline u32 textWidth(Renderer *r, const std::string &s, float size) {
    return r->drawString(s.c_str(), false, 0, 0, size, tsl::style::color::ColorTransparent).first;
}

// 膠囊：中間矩形 + 兩端半圓
inline void pill(Renderer *r, s32 x, s32 y, s32 w, s32 h, Color c) {
    const s32 rad = h / 2;
    r->drawRect(x + rad, y, w - 2 * rad, h, a(c));
    r->drawCircle(x + rad, y + rad, rad, true, a(c));
    r->drawCircle(x + w - rad - 1, y + rad, rad, true, a(c));
}

inline void pillText(Renderer *r, s32 x, s32 y, s32 w, s32 h, const std::string &s, float size, Color bg, Color fg) {
    pill(r, x, y, w, h, bg);
    const s32 tw = (s32)textWidth(r, s, size);
    r->drawString(s.c_str(), false, x + (w - tw) / 2, y + h / 2 + (s32)(size * 0.38f), size, a(fg));
}

// 一個字一個字量寬度換行（中文沒有空白，不能用空白斷行）
inline std::vector<std::string> wrapText(Renderer *r, const std::string &text, s32 maxW, float size) {
    std::vector<std::string> out;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t n = 1;
        const unsigned char c = (unsigned char)text[i];
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        const std::string ch = text.substr(i, n);
        i += n;
        if (ch == "\n") { out.push_back(line); line.clear(); continue; }
        if (!line.empty() && (s32)textWidth(r, line + ch, size) > maxW) { out.push_back(line); line.clear(); }
        line += ch;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

// ───────── 狀態條（不可選取） ─────────
enum class Tone { Ok, Warn, Error };

class StatusBar : public tsl::elm::Element {
public:
    using Provider = std::function<void(std::string &text, Tone &tone)>;
    explicit StatusBar(Provider p) : provider_(std::move(p)) {}

    void draw(Renderer *r) override {
        std::string text;
        Tone tone = Tone::Ok;
        provider_(text, tone);
        const Color bg  = tone == Tone::Ok ? kOkBg  : tone == Tone::Warn ? kWarnBg  : kErrBg;
        const Color fg  = tone == Tone::Ok ? kOkText : tone == Tone::Warn ? kWarnText : kErrText;
        const Color dot = tone == Tone::Ok ? kOkDot : tone == Tone::Warn ? kWarnDot : kErrDot;

        const s32 x = getX() + 4, y = getY() + 8, w = getWidth() - 8, h = getHeight() - 16;
        r->drawRect(x, y, w, h, a(bg));
        r->drawCircle(x + 18, y + h / 2, 6, true, a(dot));
        if (text != cacheText_) {   // 最多兩行，換行結果快取
            cacheText_ = text;
            lines_ = wrapText(r, text, w - 44, 17);
            if (lines_.size() > 2) { lines_.resize(2); lines_[1] = r->limitStringLength(lines_[1] + "…", false, 17, w - 44); }
        }
        if (lines_.size() <= 1) {
            r->drawString(lines_.empty() ? "" : lines_[0].c_str(), false, x + 34, y + h / 2 + 7, 17, a(fg));
        } else {
            r->drawString(lines_[0].c_str(), false, x + 34, y + h / 2 - 5, 17, a(fg));
            r->drawString(lines_[1].c_str(), false, x + 34, y + h / 2 + 19, 17, a(fg));
        }
    }
    void layout(u16, u16, u16, u16) override { setBoundaries(getX(), getY(), getWidth(), 76); }

private:
    Provider provider_;
    std::string cacheText_;
    std::vector<std::string> lines_;
};

// ───────── 分類膠囊（←→ 切換） ─────────
class CategoryChips : public tsl::elm::Element {
public:
    using Counter = std::function<size_t(const std::string &)>;
    using Changed = std::function<void(const std::string &)>;

    CategoryChips(std::vector<std::string> cats, Counter counter, Changed changed)
        : cats_(std::move(cats)), counter_(std::move(counter)), changed_(std::move(changed)) {}

    const std::string &current() const { return cats_[sel_]; }

    void draw(Renderer *r) override {
        const s32 h = 38, gap = 8, y = getY() + (getHeight() - h) / 2;
        // 先量每個膠囊寬度，確保選中的那個在畫面內
        std::vector<std::string> labels;
        std::vector<s32> widths;
        s32 selStart = 0, selEnd = 0, cursor = 0;
        for (size_t i = 0; i < cats_.size(); ++i) {
            labels.push_back(cats_[i] + " " + std::to_string(counter_(cats_[i])));
            widths.push_back((s32)textWidth(r, labels.back(), 17) + 28);
            if (i == sel_) { selStart = cursor; selEnd = cursor + widths.back(); }
            cursor += widths.back() + gap;
        }
        const s32 viewW = getWidth() - 8;
        if (selEnd - scroll_ > viewW) scroll_ = selEnd - viewW;
        if (selStart < scroll_) scroll_ = selStart;

        r->enableScissoring(getX(), getY(), getWidth(), getHeight());
        s32 x = getX() + 4 - scroll_;
        for (size_t i = 0; i < cats_.size(); ++i) {
            const bool on = i == sel_;
            const bool empty = counter_(cats_[i]) == 0;
            pillText(r, x, y, widths[i], h, labels[i], 17,
                     on ? kAccent : kPillOff, on ? kAccentText : (empty ? kSub : kPillOffTxt));
            x += widths[i] + gap;
        }
        r->disableScissoring();
    }

    void layout(u16, u16, u16, u16) override { setBoundaries(getX(), getY(), getWidth(), 62); }

    tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override { return this; }

    bool onClick(u64 keys) override {
        if (keys & HidNpadButton_AnyLeft)  { move(-1); return true; }
        if (keys & HidNpadButton_AnyRight) { move(+1); return true; }
        return false;
    }

private:
    void move(int d) {
        const size_t n = cats_.size();
        sel_ = (sel_ + n + d) % n;
        changed_(cats_[sel_]);
    }

    std::vector<std::string> cats_;
    Counter counter_;
    Changed changed_;
    size_t sel_ = 0;
    s32 scroll_ = 0;
};

// ───────── 金手指列（中文名 + 英文原名 + 開關膠囊） ─────────
class CheatRow : public tsl::elm::Element {
public:
    struct State { bool found; bool enabled; bool master; std::string zh; std::string en; };
    using Query = std::function<State(u32 id)>;
    using Toggle = std::function<void(u32 id, bool on)>;

    CheatRow(u32 id, Query q, Toggle t) : id_(id), query_(std::move(q)), toggle_(std::move(t)) {}

    void draw(Renderer *r) override {
        const State s = query_(id_);
        r->drawRect(getX(), getBottomBound() - 1, getWidth(), 1, a(kLine));
        if (!s.found) return;

        const s32 pillW = 74, pillH = 32;
        const s32 textMax = getWidth() - pillW - 40;
        if (cacheZh_ != s.zh) {   // 截斷結果快取起來，不用每幀重算
            cacheZh_ = s.zh;
            shownZh_ = r->limitStringLength(s.zh, false, 21, textMax);
            shownEn_ = (s.en != s.zh) ? r->limitStringLength(s.en, false, 14, textMax) : "";
        }
        const bool twoLine = !shownEn_.empty();
        r->drawString(shownZh_.c_str(), false, getX() + 16, getY() + (twoLine ? 33 : 44), 21, a(kText));
        if (twoLine) r->drawString(shownEn_.c_str(), false, getX() + 16, getY() + 57, 14, a(kSub));

        const s32 px = getX() + getWidth() - pillW - 12, py = getY() + (getHeight() - pillH) / 2;
        if (s.master) pillText(r, px, py, pillW, pillH, "主碼", 16, kPillOff, kSub);
        else pillText(r, px, py, pillW, pillH, s.enabled ? "開啟" : "關閉", 16,
                      s.enabled ? kAccent : kPillOff, s.enabled ? kAccentText : kPillOffTxt);
    }

    void layout(u16, u16, u16, u16) override { setBoundaries(getX(), getY(), getWidth(), 72); }

    tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override { return this; }

    bool onClick(u64 keys) override {
        if (!(keys & HidNpadButton_A)) return false;
        const State s = query_(id_);
        if (!s.found || s.master) return true;
        triggerClickAnimation();
        toggle_(id_, !s.enabled);
        return true;
    }

    u32 id() const { return id_; }

private:
    u32 id_;
    Query query_;
    Toggle toggle_;
    std::string cacheZh_, shownZh_, shownEn_;
};

// ───────── 數字鍵盤（十字鍵選鍵 + A 輸入，或直接用手指點） ─────────
class Keypad : public tsl::elm::Element {
public:
    using Done = std::function<void(const std::string &)>;

    Keypad(std::string initial, bool allowFloat, Done done)
        : text_(initial.empty() ? "0" : std::move(initial)), allowFloat_(allowFloat), done_(std::move(done)) {}

    void draw(Renderer *r) override {
        // 輸入框
        const s32 boxH = 74;
        r->drawRect(getX(), getY(), getWidth(), boxH, a(kPillOff));
        const std::string shown = text_ + "_";
        const s32 tw = (s32)textWidth(r, shown, 40);
        r->drawString(shown.c_str(), false, getX() + getWidth() - tw - 16, getY() + 53, 40, a(kText));
        r->drawString(allowFloat_ ? "可輸入小數" : "整數", false, getX() + 10, getY() + 22, 14, a(kSub));

        // 按鍵 4 × 4
        const s32 gap = 8, top = getY() + boxH + 16;
        keyW_ = (getWidth() - gap * 3) / 4;
        keyH_ = (getBottomBound() - top - 40 - gap * 3) / 4;
        if (keyH_ > 78) keyH_ = 78;
        top_ = top;
        for (int i = 0; i < 16; ++i) {
            const s32 x = getX() + (i % 4) * (keyW_ + gap), y = top + (i / 4) * (keyH_ + gap);
            const bool sel = focused_ && i == sel_;
            const bool disabled = !enabled(i);
            const bool ok = std::string(kKeys[i]) == "確定";
            const Color bg = sel ? kAccent : ok ? kOkBg : kPillOff;
            const Color fg = sel ? kAccentText : disabled ? kLine : ok ? kOkText : kText;
            r->drawRect(x, y, keyW_, keyH_, a(bg));
            const float fs = std::strlen(kKeys[i]) > 2 ? 20 : 28;
            const s32 kw = (s32)textWidth(r, kKeys[i], fs);
            r->drawString(kKeys[i], false, x + (keyW_ - kw) / 2, y + keyH_ / 2 + (s32)(fs * 0.38f), fs, a(fg));
        }
        r->drawString("A 輸入　X 刪除　+ 確定　B 取消", false, getX() + 4, getBottomBound() - 8, 15, a(kSub));
    }

    void layout(u16, u16, u16, u16) override {}
    tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override { return this; }
    void setFocused(bool f) override { focused_ = f; Element::setFocused(f); }
    // 自己畫選取框，不要 libtesla 預設那個包住整個鍵盤的大框
    void drawHighlight(Renderer *) override {}

    bool onClick(u64 keys) override {
        if (keys & HidNpadButton_AnyLeft)  { if (sel_ % 4 > 0) --sel_; return true; }
        if (keys & HidNpadButton_AnyRight) { if (sel_ % 4 < 3) ++sel_; return true; }
        if (keys & HidNpadButton_AnyUp)    { if (sel_ >= 4) sel_ -= 4; return true; }
        if (keys & HidNpadButton_AnyDown)  { if (sel_ < 12) sel_ += 4; return true; }
        if (keys & HidNpadButton_X)        { press(3); return true; }       // 刪除
        if (keys & HidNpadButton_Plus)     { return finish(); }
        if (keys & HidNpadButton_A)        { return press(sel_); }
        return false;                                                       // B 交給 libtesla 返回上一頁
    }

    bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32, s32, s32, s32) override {
        if (e != tsl::elm::TouchEvent::Release || keyW_ == 0) return false;
        for (int i = 0; i < 16; ++i) {
            const s32 x = getX() + (i % 4) * (keyW_ + 8), y = top_ + (i / 4) * (keyH_ + 8);
            if (cx >= x && cx < x + keyW_ && cy >= y && cy < y + keyH_) { sel_ = i; return press(i); }
        }
        return false;
    }

private:
    static constexpr const char *kKeys[16] = {
        "7", "8", "9", "⌫",
        "4", "5", "6", "清除",
        "1", "2", "3", "±",
        ".", "0", "00", "確定",
    };

    bool enabled(int i) const { return std::string(kKeys[i]) != "." || allowFloat_; }

    bool press(int i) {
        const std::string k = kKeys[i];
        if (!enabled(i)) return true;
        if (k == "確定") return finish();
        if (k == "⌫") { text_.pop_back(); if (text_.empty() || text_ == "-") text_ = "0"; return true; }
        if (k == "清除") { text_ = "0"; return true; }
        if (k == "±") { if (text_[0] == '-') text_.erase(0, 1); else if (text_ != "0") text_.insert(0, "-"); return true; }
        if (text_.size() >= 20) return true;
        if (k == ".") { if (text_.find('.') == std::string::npos) text_ += "."; return true; }
        // 數字：開頭的 0 直接換掉
        if (text_ == "0") text_ = (k == "00") ? "0" : k;
        else if (text_ == "-0") text_ = (k == "00") ? "-0" : "-" + k;
        else text_ += k;
        return true;
    }

    // 確定：先把要用的東西複製出來，再返回上一頁（返回後這個元件就被刪掉了）
    bool finish() {
        Done cb = done_;
        std::string t = text_;
        if (!t.empty() && t.back() == '.') t.pop_back();
        tsl::goBack();
        cb(t);
        return true;
    }

    std::string text_;
    bool allowFloat_;
    Done done_;
    int sel_ = 13;          // 預設停在 0
    bool focused_ = false;
    s32 keyW_ = 0, keyH_ = 0, top_ = 0;
};

// ───────── 說明框：固定在畫面下方，跟著游標顯示目前選項的說明 ─────────
class HelpBox : public tsl::elm::Element {
public:
    using Provider = std::function<std::string()>;
    explicit HelpBox(Provider p) : provider_(std::move(p)) {}

    void draw(Renderer *r) override {
        const std::string text = provider_();
        if (text != cacheText_ || getWidth() != cacheWidth_) {   // 換行結果快取，文字沒變就不重算
            cacheText_ = text;
            cacheWidth_ = getWidth();
            lines_ = wrapText(r, text, getWidth() - 28, kFont);
        }
        r->drawRect(getX(), getY() + 6, getWidth(), 1, a(kLine));
        r->drawString("說明", false, getX() + 4, getY() + 28, 15, a(kAccent));
        s32 y = getY() + 52;
        for (const auto &line : lines_) {
            if (y > getBottomBound() - 4) break;
            r->drawString(line.c_str(), false, getX() + 4, y, kFont, a(kSub));
            y += kLineH;
        }
    }
    // 大小由外層 SplitView 決定，這裡不改
    void layout(u16, u16, u16, u16) override {}

private:
    static constexpr float kFont = 17;
    static constexpr s32 kLineH = 24;

    Provider provider_;
    std::string cacheText_;
    u16 cacheWidth_ = 0;
    std::vector<std::string> lines_;
};

// ───────── 上下分割：上面是可捲動的列表，下面固定一塊（說明框） ─────────
class SplitView : public tsl::elm::Element {
public:
    SplitView(tsl::elm::Element *top, tsl::elm::Element *bottom, u16 bottomHeight)
        : top_(top), bottom_(bottom), bottomH_(bottomHeight) {
        top_->setParent(this);
        bottom_->setParent(this);
    }
    ~SplitView() override { delete top_; delete bottom_; }

    void draw(Renderer *r) override {
        top_->frame(r);
        bottom_->frame(r);
    }
    void layout(u16, u16, u16, u16) override {
        top_->setBoundaries(getX(), getY(), getWidth(), getHeight() - bottomH_);
        top_->invalidate();
        bottom_->setBoundaries(getX(), getY() + getHeight() - bottomH_, getWidth(), bottomH_);
        bottom_->invalidate();
    }
    tsl::elm::Element *requestFocus(tsl::elm::Element *old, tsl::FocusDirection d) override {
        return top_->requestFocus(old, d);
    }
    bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32 px, s32 py, s32 ix, s32 iy) override {
        return top_->onTouch(e, cx, cy, px, py, ix, iy);
    }

private:
    tsl::elm::Element *top_, *bottom_;
    u16 bottomH_;
};

} // namespace ui
