// 進階功能的背景工作：記憶體搜尋、指標搜尋、指標篩選、跨版本移植
// 全部在背景執行緒跑，畫面不會卡住；同一時間只跑一個工作
#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <switch.h>
#include "search_engine.hpp"
#include "pointer_search.hpp"
#include "porter.hpp"

namespace cheattw {

// 搜尋範圍：
//   Data     遊戲資料區 = Heap + Alias（很多新遊戲的資料放在 Alias 區，不在 Heap）
//   WithMain 再加上主程式的資料段
//   All      全部可讀寫的記憶體（最慢，前兩個找不到時用）
//   MainOnly 只有主程式（內部用：跨版本移植找起點）
enum class SearchScope : u8 { Data, WithMain, All, MainOnly };

// 畫面上的搜尋設定，跟著搜尋進度一起存檔
struct SearchSettings {
    SearchMode mode = SearchMode::Smart;
    ValueType type = ValueType::U32;
    Cond cond = Cond::Equal;
    char value[32] = "100";
    char value2[32] = "";
    char value3[32] = "";
    SearchScope scope = SearchScope::Data;
};

enum class RestoreResult : u8 { None, Restored, RestoredStale };
enum class JobKind : u8 { None, Scan, PointerSearch, PointerFilter, Port };

class SearchTask {
public:
    // ── 記憶體搜尋 ──
    void persist(const SearchSettings &ui);
    RestoreResult restore(SearchSettings &ui);
    void clearSaved();
    // 第一次：spec 完整；接著搜：只看 spec.cond、spec.value。失敗時 why 會填原因
    bool startScan(bool first, const ScanSpec &spec, SearchScope scope, std::string *why = nullptr);
    void reset();

    // ── 指標搜尋：找出能到達 target 的指標路徑（存 SD 卡），重開遊戲後用新位址篩選 ──
    bool startPointerSearch(u64 target, const Encoding &enc, std::string *why = nullptr);
    bool startPointerFilter(u64 newTarget, std::string *why = nullptr);
    size_t pointerCount() const;          // 目前存著的指標路徑數（0 = 沒有或不是這個遊戲）
    Encoding pointerEncoding() const;     // 指標路徑對應的數值存法
    std::string pointerPath() const;

    // ── 跨版本移植：拿 db/{TID}/other/ 裡其他版本的碼 ──
    bool startPort(std::string *why = nullptr);
    const std::vector<PortResult> &portResults() const { return portResults_; }

    // ── 共用 ──
    std::string summary() const { return summary_; }
    void cancel() { progress_.cancel = true; }
    // overlay 結束前呼叫：等背景工作結束
    void join() { if (threadValid_) { threadWaitForExit(&thread_); threadClose(&thread_); threadValid_ = false; } }
    bool running() const { return running_; }
    JobKind runningJob() const { return running_ ? job_ : JobKind::None; }
    JobKind lastJob() const { return job_; }
    // 工作結束後第一次呼叫回傳 true（畫面用來更新一次）
    bool takeFinished() { return finished_.exchange(false); }
    SearchResult lastResult() const { return last_; }
    SearchProgress &progress() { return progress_; }
    SearchEngine &engine();

    // 位址 → 金手指用的記憶體區與偏移；回傳 false 表示不在已知區段
    bool locate(u64 addr, u8 &regionType, u64 &offset, std::string &label) const;

private:
    bool launch(JobKind job, std::string *why);
    static void threadMain(void *arg);
    void runScan();
    void runPointerSearch();
    void runPointerFilter();
    void runPort();

    Thread thread_{};
    bool threadValid_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    SearchProgress progress_;
    SearchResult last_ = SearchResult::NotStarted;
    JobKind job_ = JobKind::None;

    bool first_ = true;
    ScanSpec spec_;
    SearchScope scope_ = SearchScope::Data;
    u64 ptrTarget_ = 0;
    Encoding ptrEnc_ = {ValueType::U32, 1};
    std::vector<PortResult> portResults_;
    char summary_[160] = {};
};

SearchTask &searchTask();

} // namespace cheattw
