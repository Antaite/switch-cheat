// 進階搜尋：把 SearchEngine 接到 dmnt，並在背景執行緒跑，畫面不會卡住
#pragma once
#include <atomic>
#include <string>
#include <switch.h>
#include "search_engine.hpp"

namespace cheattw {

// 搜尋範圍：
//   Data     遊戲資料區 = Heap + Alias（很多新遊戲的資料放在 Alias 區，不在 Heap）
//   WithMain 再加上主程式的資料段
//   All      全部可讀寫的記憶體（最慢，前兩個找不到時用）
enum class SearchScope : u8 { Data, WithMain, All };

class SearchTask {
public:
    // 開始搜尋（第一次或接著搜），立刻回傳；進度看 progress()
    // 失敗時 why 會填原因（中文）
    bool start(bool first, ValueType type, Cond cond, u64 valueBits, SearchScope scope, std::string *why = nullptr);
    // 搜完的補充說明（區段數、掃了多少、花幾秒），顯示在狀態條
    std::string summary() const { return summary_; }
    void cancel() { progress_.cancel = true; }
    // overlay 結束前呼叫：等背景搜尋結束
    void join() { if (threadValid_) { threadWaitForExit(&thread_); threadClose(&thread_); threadValid_ = false; } }
    void reset();

    bool running() const { return running_; }
    // 搜完後第一次呼叫回傳 true（畫面用來更新一次）
    bool takeFinished() { return finished_.exchange(false); }
    SearchResult lastResult() const { return last_; }
    SearchProgress &progress() { return progress_; }
    SearchEngine &engine();

    // 位址 → 金手指用的記憶體區與偏移；回傳 false 表示不在已知區段
    bool locate(u64 addr, u8 &regionType, u64 &offset, std::string &label) const;

private:
    static void threadMain(void *arg);

    Thread thread_{};
    bool threadValid_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    SearchProgress progress_;
    SearchResult last_ = SearchResult::NotStarted;

    bool first_ = true;
    ValueType type_ = ValueType::U32;
    Cond cond_ = Cond::Equal;
    u64 value_ = 0;
    SearchScope scope_ = SearchScope::Data;
    char summary_[160] = {};
};

SearchTask &searchTask();

} // namespace cheattw
