#include "search_task.hpp"
#include <algorithm>
#include <cstdio>
#include "cheat_session.hpp"
#include "dmntcht.h"

namespace cheattw {

namespace {

// 引擎約 144KB，一定要放全域，不能放 stack（背景執行緒 stack 只有 16KB）
SearchEngine g_engine;

class DmntReader : public MemoryReader {
public:
    explicit DmntReader(SearchScope s) : scope_(s) {}

    size_t regions(Region *out, size_t max) override {
        const auto &e = session().extents();
        size_t n = 0;
        if (scope_ == SearchScope::MainAndHeap) n = collect(e.mainBase, e.mainSize, out, n, max);
        n = collect(e.heapBase, e.heapSize, out, n, max);
        return n;
    }

    bool read(uint64_t addr, void *buf, size_t size) override {
        return R_SUCCEEDED(dmntchtReadCheatProcessMemory(addr, buf, size));
    }

private:
    // 只收可讀寫、有對應實體記憶體的區段
    static size_t collect(u64 base, u64 size, Region *out, size_t n, size_t max) {
        const u64 end = base + size;
        u64 addr = base;
        while (addr < end && n < max) {
            MemoryInfo mi{};
            if (R_FAILED(dmntchtQueryCheatProcessMemory(&mi, addr)) || mi.size == 0) break;
            const u64 rs = std::max<u64>(mi.addr, base);
            const u64 re = std::min<u64>(mi.addr + mi.size, end);
            const bool usable = (mi.perm & Perm_Rw) == Perm_Rw && mi.type != MemType_Unmapped &&
                                mi.type != MemType_Io && mi.type != MemType_Reserved;
            if (usable && re > rs) {
                // 相鄰的區段合併，減少區段數
                if (n > 0 && out[n - 1].base + out[n - 1].size == rs) out[n - 1].size += re - rs;
                else out[n++] = {rs, re - rs};
            }
            const u64 next = mi.addr + mi.size;
            if (next <= addr) break;   // 防止無窮迴圈
            addr = next;
        }
        return n;
    }

    SearchScope scope_;
};

} // namespace

SearchTask &searchTask() {
    static SearchTask t;
    return t;
}

SearchEngine &SearchTask::engine() { return g_engine; }

bool SearchTask::start(bool first, ValueType type, Cond cond, u64 valueBits, SearchScope scope, std::string *why) {
    auto fail = [why](const char *m) { if (why) *why = m; return false; };
    if (running_) return fail("已經在搜尋了");
    if (!session().isOpen()) return fail("dmnt 沒有連線，請回首頁按「重新連線」");
    if (!session().hasGame()) return fail("沒有執行中的遊戲");
    if (session().extents().heapSize == 0) return fail("讀不到遊戲的記憶體範圍");
    if (threadValid_) { threadWaitForExit(&thread_); threadClose(&thread_); threadValid_ = false; }

    mkdirs(kSearchDir);
    g_engine.setWorkDir(kSearchDir);
    first_ = first; type_ = type; cond_ = cond; value_ = valueBits; scope_ = scope;
    progress_.cancel = false;
    progress_.done = 0;
    progress_.total = 0;

    session().setKeepAlive(true);   // 搜尋中 overlay 被隱藏也不能斷線
    running_ = true;
    summary_[0] = '\0';
    // 大緩衝都在全域，64KB stack 很充裕
    if (R_FAILED(threadCreate(&thread_, threadMain, this, nullptr, 0x10000, 0x2C, -2)) ||
        R_FAILED(threadStart(&thread_))) {
        running_ = false;
        session().setKeepAlive(false);
        return fail("無法建立搜尋執行緒（記憶體不足）");
    }
    threadValid_ = true;
    return true;
}

void SearchTask::threadMain(void *arg) {
    auto *self = static_cast<SearchTask *>(arg);
    DmntReader reader(self->scope_);
    const u64 t0 = armTicksToNs(armGetSystemTick());

    // 暫停遊戲，數值才不會在搜尋途中變動；搜完一定要恢復
    dmntchtPauseCheatProcess();
    self->last_ = self->first_
        ? g_engine.firstScan(reader, self->type_, self->cond_, self->value_, self->progress_)
        : g_engine.nextScan(reader, self->cond_, self->value_, self->progress_);
    dmntchtResumeCheatProcess();

    // 記錄：狀態條顯示一行摘要，詳細寫到 switch/CheatTW/search.log
    const u64 sec = (armTicksToNs(armGetSystemTick()) - t0) / 1000000000ull;
    const u64 mb = self->first_ ? self->progress_.total.load() / (1024 * 1024) : 0;
    if (self->first_) std::snprintf(self->summary_, sizeof(self->summary_), "掃描 %llu MB，花 %llu 秒", (unsigned long long)mb, (unsigned long long)sec);
    else std::snprintf(self->summary_, sizeof(self->summary_), "比對 %llu 筆，花 %llu 秒", (unsigned long long)self->progress_.total.load(), (unsigned long long)sec);
    if (FILE *f = std::fopen((std::string(kAppDir) + "/search.log").c_str(), "a")) {
        const auto &e = session().extents();
        std::fprintf(f, "%s type=%d cond=%d value=%llu heap=%llx+%llx 結果=%s 筆數=%llu %s\n",
                     self->first_ ? "第一次" : "接著搜", (int)self->type_, (int)self->cond_,
                     (unsigned long long)self->value_, (unsigned long long)e.heapBase, (unsigned long long)e.heapSize,
                     searchResultText(self->last_), (unsigned long long)g_engine.count(), self->summary_);
        std::fclose(f);
    }

    session().setKeepAlive(false);
    self->running_ = false;
    self->finished_ = true;
}

void SearchTask::reset() {
    if (running_) return;
    mkdirs(kSearchDir);
    g_engine.setWorkDir(kSearchDir);
    g_engine.reset();
    last_ = SearchResult::NotStarted;
}

bool SearchTask::locate(u64 addr, u8 &regionType, u64 &offset, std::string &label) const {
    const auto &e = session().extents();
    char buf[32];
    if (addr >= e.heapBase && addr < e.heapBase + e.heapSize) {
        regionType = 1; offset = addr - e.heapBase;
        std::snprintf(buf, sizeof(buf), "H+0x%010llX", (unsigned long long)offset);
    } else if (addr >= e.mainBase && addr < e.mainBase + e.mainSize) {
        regionType = 0; offset = addr - e.mainBase;
        std::snprintf(buf, sizeof(buf), "M+0x%010llX", (unsigned long long)offset);
    } else {
        return false;
    }
    label = buf;
    return true;
}

} // namespace cheattw
