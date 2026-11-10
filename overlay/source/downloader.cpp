#include "downloader.hpp"
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

// 下載到暫存檔，成功才改名，避免中途斷線留下壞檔；回傳 HTTP 狀態碼（0 = 連線失敗）
long fetch(const std::string &url, const std::string &path) {
    const std::string tmp = path + ".part";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return -1;

    CURL *curl = curl_easy_init();
    if (!curl) { std::fclose(f); std::remove(tmp.c_str()); return 0; }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CheatTW");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 16384L);   // 接收緩衝也調小
    // 主機上沒有內建憑證庫，關掉憑證驗證（只抓公開的金手指文字檔）
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
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
    return message_;
}

bool Downloader::start(const std::string &tid, const std::string &bid) {
    if (state_ == State::Running) return false;
    if (threadValid_) { threadWaitForExit(&thread_); threadClose(&thread_); threadValid_ = false; }
    tid_ = tid;
    bid_ = bid;
    filesDone_ = 0;
    std::snprintf(message_, sizeof(message_), "連線中…");
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
    self->run();
    self->finished_ = true;
}

void Downloader::run() {
    // 網路服務要在 sm 開著的時候初始化（libtesla 平常是關著的）
    Result rc = smInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = socketInitialize(&kSocketConfig);
        smExit();
    }
    if (R_FAILED(rc)) {
        std::snprintf(message_, sizeof(message_), "網路初始化失敗（%04u-%04u）",
                      2000 + (rc & 0x1FF), (rc >> 9) & 0x1FFF);
        state_ = State::Failed;
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const std::string base = baseUrl() + "db/" + tid_ + "/";
    const std::string dir = std::string(kDbDir) + "/" + tid_;
    mkdirs(dir);

    // 1. 組合清單：沒有就是線上碼庫沒收錄這個版本
    const std::string metaPath = dir + "/" + bid_ + ".meta.tsv";
    long http = fetch(base + bid_ + ".meta.tsv", metaPath);
    if (http != 200) {
        if (http == 404) {
            std::snprintf(message_, sizeof(message_), "線上碼庫沒有這個版本的碼");
            state_ = State::NotFound;
        } else {
            std::snprintf(message_, sizeof(message_), http == 0 ? "連不上網路，請確認已連上 Wi-Fi" : "下載失敗（HTTP %ld）", http);
            state_ = State::Failed;
        }
        curl_global_cleanup();
        socketExit();
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
        std::snprintf(message_, sizeof(message_), "下載中 %zu / %zu", i + 1, files.size());
        if (fetch(base + files[i], dir + "/" + files[i]) == 200) ++ok;
        filesDone_ = ok;
    }
    fetch(base + "title.txt", dir + "/title.txt");   // 遊戲中文名，沒有也沒關係

    curl_global_cleanup();
    socketExit();

    if (ok == (int)files.size() && ok > 0) {
        std::snprintf(message_, sizeof(message_), "下載完成，共 %d 個組合", ok);
        state_ = State::Done;
    } else {
        std::snprintf(message_, sizeof(message_), "部分下載失敗（%d / %zu）", ok, files.size());
        state_ = ok > 0 ? State::Done : State::Failed;
    }
}

} // namespace cheattw
