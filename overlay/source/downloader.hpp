// 線上碼庫下載：在背景執行緒用 curl 抓目前遊戲版本的碼，存到 switch/CheatTW/db/
//
// 記憶體：overlay 的 heap 很小，所以網路緩衝特別調小（約 100KB，預設會用到 2MB），
//         下載時直接寫進 SD 卡，不把整個檔案放在記憶體。
#pragma once
#include <atomic>
#include <string>
#include <switch.h>

namespace cheattw {

class Downloader {
public:
    enum class State : u8 { Idle, Running, Done, NotFound, Failed };

    // 開始下載這個遊戲版本（已經在下載就忽略）
    bool start(const std::string &tid, const std::string &bid);
    State state() const { return state_; }
    bool running() const { return state_ == State::Running; }
    // 狀態從 Running 變成結束後，第一次呼叫回傳 true（畫面用來更新一次）
    bool takeFinished() { return finished_.exchange(false); }
    std::string message() const;      // 目前進度或結果（中文）
    int filesDone() const { return filesDone_; }

    // overlay 結束前呼叫：等下載執行緒結束（curl 最多 30 秒逾時）
    void join() { if (threadValid_) { threadWaitForExit(&thread_); threadClose(&thread_); threadValid_ = false; } }

    // 碼庫網址，可在 switch/CheatTW/config.ini 用 db_url= 改
    static std::string baseUrl();

private:
    static void threadMain(void *arg);
    void run();

    Thread thread_{};
    bool threadValid_ = false;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> finished_{false};
    std::atomic<int> filesDone_{0};
    std::string tid_, bid_;
    char message_[128] = {};
};

Downloader &downloader();

} // namespace cheattw
