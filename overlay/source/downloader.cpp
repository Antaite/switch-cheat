#include "downloader.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <curl/curl.h>
#include "cheat_session.hpp"

namespace cheattw {

namespace {

constexpr const char *kDefaultBaseUrl = "https://raw.githubusercontent.com/Antaite/switch-cheat/gh-pages/";

// 網路緩衝調小：預設設定會跟系統要約 2MB 的傳輸記憶體，overlay 承受不起
const SocketInitConfig kSocketConfig = {
    .tcp_tx_buf_size     = 0x4000,
    .tcp_rx_buf_size     = 0x8000,
    .tcp_tx_buf_max_size = 0x4000,
    .tcp_rx_buf_max_size = 0x8000,
    .udp_tx_buf_size     = 0x2400,     // DNS 查詢要用到 UDP
    .udp_rx_buf_size     = 0xA500,
    .sb_efficiency       = 1,
    .num_bsd_sessions    = 1,
    .bsd_service_type    = BsdServiceType_User,
};

size_t writeToFile(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE *>(userdata));
}

FILE *g_log = nullptr;   // switch/CheatTW/download.log，回報問題用

void logf(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

// 進度回呼：超過總時限就中止（避免卡死）
struct XferCtx { u64 deadline; };
int onProgress(void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return armTicksToNs(armGetSystemTick()) > static_cast<XferCtx *>(p)->deadline ? 1 : 0;
}

// 下載到暫存檔，成功才改名，避免中途斷線留下壞檔；回傳 HTTP 狀態碼（0 = 連線失敗）
long fetch(const std::string &url, const std::string &path, std::string *err = nullptr) {
    const std::string tmp = path + ".part";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) { logf("無法寫入 %s", tmp.c_str()); if (err) *err = "SD 卡無法寫入"; return -1; }

    CURL *curl = curl_easy_init();
    if (!curl) { std::fclose(f); std::remove(tmp.c_str()); if (err) *err = "curl 初始化失敗"; return 0; }
    char errbuf[CURL_ERROR_SIZE] = {};
    XferCtx ctx{armTicksToNs(armGetSystemTick()) + 40ull * 1000000000ull};   // 每個檔最多 40 秒
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CheatTW");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 16384L);   // 接收緩衝也調小
    // 只抓公開的金手指文字檔，關掉憑證驗證避免主機時間不準時連不上
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    if (g_log) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_STDERR, g_log);
    }

    logf("GET %s", url.c_str());
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    logf("→ curl=%d http=%ld %s", (int)rc, http, errbuf);
    if (err && rc != CURLE_OK) *err = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    curl_easy_cleanup(curl);
    std::fclose(f);

    if (http == 200) {
        std::remove(path.c_str());
        std::rename(tmp.c_str(), path.c_str());
    } else {
        std::remove(tmp.c_str());
    }
    return http;
}

} // namespace

Downloader &downloader() {
    static Downloader d;
    return d;
}

std::string Downloader::baseUrl() {
    std::string url = kDefaultBaseUrl;
    if (FILE *f = std::fopen((std::string(kAppDir) + "/config.ini").c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "db_url=", 7) != 0) continue;
            url = line + 7;
            while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == ' ')) url.pop_back();
            if (!url.empty() && url.back() != '/') url += '/';
        }
        std::fclose(f);
    }
    return url;
}

std::string Downloader::message() const {
    std::string m = message_;
    if (state_ == State::Running) {
        const u64 sec = (armTicksToNs(armGetSystemTick()) - startNs_) / 1000000000ull;
        m += "（" + std::to_string(sec) + " 秒）";
    }
    return m;
}

void Downloader::setStage(const char *text) {
    std::snprintf(message_, sizeof(message_), "%s", text);
    logf("[%s]", text);
}

void Downloader::fail(const char *what, Result rc) {
    if (rc) std::snprintf(message_, sizeof(message_), "%s（%04u-%04u）", what, 2000 + (rc & 0x1FF), (rc >> 9) & 0x1FFF);
    else std::snprintf(message_, sizeof(message_), "%s", what);
    logf("失敗：%s", message_);
    state_ = State::Failed;
    if (g_log) { std::fclose(g_log); g_log = nullptr; }
}

void Downloader::cleanup() {
    if (finishCleanup_) {
        curl_global_cleanup();
        socketExit();
        smExit();
        finishCleanup_ = false;
    }
    logf("結果：%s", message_);
    if (g_log) { std::fclose(g_log); g_log = nullptr; }
}

bool Downloader::startOthers(const std::string &tid, const std::string &bid) {
    others_ = true;
    if (!start(tid, bid)) { others_ = false; return false; }
    return true;
}

bool Downloader::start(const std::string &tid, const std::string &bid) {
    if (state_ == State::Running) return false;
    if (threadValid_) { threadWaitForExit(&thread_); threadClose(&thread_); threadValid_ = false; }
    tid_ = tid;
    bid_ = bid;
    filesDone_ = 0;
    startNs_ = armTicksToNs(armGetSystemTick());
    std::snprintf(message_, sizeof(message_), "準備中…");
    state_ = State::Running;
    // curl + TLS 要的 stack 比較多，給 64KB
    if (R_FAILED(threadCreate(&thread_, threadMain, this, nullptr, 0x10000, 0x2C, -2)) ||
        R_FAILED(threadStart(&thread_))) {
        std::snprintf(message_, sizeof(message_), "無法建立下載執行緒");
        state_ = State::Failed;
        finished_ = true;
        return false;
    }
    threadValid_ = true;
    return true;
}

void Downloader::threadMain(void *arg) {
    auto *self = static_cast<Downloader *>(arg);
    if (self->others_) self->runOthers();
    else self->run();
    self->others_ = false;
    self->finished_ = true;
}

bool Downloader::beginNetwork() {
    mkdirs(kAppDir);
    g_log = std::fopen((std::string(kAppDir) + "/download.log").c_str(), "w");
    logf("CheatTW 下載記錄  TID %s  BID %s%s", tid_.c_str(), bid_.c_str(), others_ ? "（其他版本）" : "");

    // curl 的 SSL 是用系統的 ssl 服務，初始化和每次連線都要跟 sm 拿服務，
    // 所以整個下載期間都要開著 sm（libtesla 平常是關著的）
    setStage("初始化網路…");
    Result rc = smInitialize();
    if (R_FAILED(rc)) { fail("系統服務無法使用", rc); return false; }
    rc = socketInitialize(&kSocketConfig);
    if (R_FAILED(rc)) { smExit(); fail("網路初始化失敗", rc); return false; }
    const CURLcode grc = curl_global_init(CURL_GLOBAL_DEFAULT);
    logf("curl_global_init=%d", (int)grc);
    if (grc != CURLE_OK) {
        socketExit();
        smExit();
        fail("SSL 初始化失敗", 0);
        return false;
    }
    finishCleanup_ = true;
    return true;
}

void Downloader::runOthers() {
    if (!beginNetwork()) return;
    const std::string base = baseUrl() + "db/" + tid_ + "/";
    const std::string dir = std::string(kDbDir) + "/" + tid_;
    mkdirs(dir + "/other");

    setStage("讀取版本清單…");
    std::string err;
    const long http = fetch(base + "builds.txt", dir + "/builds.txt", &err);
    if (http != 200) {
        if (http == 404) std::snprintf(message_, sizeof(message_), "線上碼庫沒有這款遊戲");
        else if (http == 0) std::snprintf(message_, sizeof(message_), "連線失敗：%s", err.empty() ? "請確認已連上 Wi-Fi" : err.c_str());
        else std::snprintf(message_, sizeof(message_), "下載失敗（HTTP %ld）", http);
        cleanup();
        state_ = http == 404 ? State::NotFound : State::Failed;
        return;
    }
    std::vector<std::string> bids;
    if (FILE *f = std::fopen((dir + "/builds.txt").c_str(), "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            std::string b(line, strcspn(line, "\t\r\n"));
            if (b.size() == 16 && b != bid_) bids.push_back(b);
        }
        std::fclose(f);
    }
    int ok = 0;
    for (size_t i = 0; i < bids.size(); ++i) {
        char st[64];
        std::snprintf(st, sizeof(st), "下載其他版本 %zu / %zu", i + 1, bids.size());
        setStage(st);
        if (fetch(base + bids[i] + ".txt", dir + "/other/" + bids[i] + ".txt") == 200) ++ok;
        filesDone_ = ok;
    }
    if (bids.empty()) std::snprintf(message_, sizeof(message_), "線上碼庫只有你這個版本，沒有其他版本可以移植");
    else std::snprintf(message_, sizeof(message_), "已下載 %d 個其他版本的碼", ok);
    cleanup();
    state_ = ok > 0 ? State::Done : (bids.empty() ? State::NotFound : State::Failed);
}

void Downloader::run() {
    if (!beginNetwork()) return;

    const std::string base = baseUrl() + "db/" + tid_ + "/";
    const std::string dir = std::string(kDbDir) + "/" + tid_;
    mkdirs(dir);

    // 1. 組合清單：沒有就是線上碼庫沒收錄這個版本
    const std::string metaPath = dir + "/" + bid_ + ".meta.tsv";
    setStage("連線到碼庫…");
    std::string err;
    long http = fetch(base + bid_ + ".meta.tsv", metaPath, &err);
    if (http != 200) {
        if (http == 404) std::snprintf(message_, sizeof(message_), "線上碼庫沒有這個版本的碼");
        else if (http == 0) std::snprintf(message_, sizeof(message_), "連線失敗：%s", err.empty() ? "請確認已連上 Wi-Fi" : err.c_str());
        else std::snprintf(message_, sizeof(message_), "下載失敗（HTTP %ld）", http);
        cleanup();
        state_ = http == 404 ? State::NotFound : State::Failed;
        return;
    }

    // 2. 依清單下載每個組合的碼
    std::vector<std::string> files;
    if (FILE *f = std::fopen(metaPath.c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            char *tab = std::strchr(line, '\t');
            if (tab) { *tab = '\0'; files.push_back(line); }
        }
        std::fclose(f);
    }
    int ok = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        char st[64];
        std::snprintf(st, sizeof(st), "下載中 %zu / %zu", i + 1, files.size());
        setStage(st);
        if (fetch(base + files[i], dir + "/" + files[i]) == 200) ++ok;
        filesDone_ = ok;
    }
    fetch(base + "title.txt", dir + "/title.txt");   // 遊戲中文名，沒有也沒關係

    // 先寫好結果訊息再收尾，狀態最後才改，畫面看到結束時訊息一定是最終的
    const bool allOk = ok == (int)files.size() && ok > 0;
    if (allOk) std::snprintf(message_, sizeof(message_), "下載完成，共 %d 個組合，請選一個載入", ok);
    else std::snprintf(message_, sizeof(message_), "部分下載失敗（%d / %zu）", ok, files.size());
    cleanup();
    state_ = (allOk || ok > 0) ? State::Done : State::Failed;
}

} // namespace cheattw
