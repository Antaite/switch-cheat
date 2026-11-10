// 自訂畫面元件：照示意圖的風格（狀態條、分類膠囊、雙行金手指 + 開關膠囊）
#pragma once
#include <tesla.hpp>
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
        const std::string shown = r->limitStringLength(text, false, 18, w - 44);
        r->drawString(shown.c_str(), false, x + 34, y + h / 2 + 7, 18, a(fg));
    }
    void layout(u16, u16, u16, u16) override { setBoundaries(getX(), getY(), getWidth(), 58); }

private:
    Provider provider_;
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
            lines_ = wrap(r, text, getWidth() - 28, kFont);
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

    // 一個字一個字量寬度，超過就換行（中文沒有空白，不能用空白斷行）
    static std::vector<std::string> wrap(Renderer *r, const std::string &text, s32 maxW, float size) {
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
