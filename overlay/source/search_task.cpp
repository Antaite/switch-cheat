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

bool SearchTask::start(bool first, ValueType type, Cond cond, u64 valueBits, SearchScope scope) {
    if (running_ || !session().hasGame()) return false;
    if (threadValid_) { threadWaitForExit(&thread_); threadClose(&thread_); threadValid_ = false; }

    mkdirs(kSearchDir);
    g_engine.setWorkDir(kSearchDir);
    first_ = first; type_ = type; cond_ = cond; value_ = valueBits; scope_ = scope;
    progress_.cancel = false;
    progress_.done = 0;
    progress_.total = 0;

    session().setKeepAlive(true);   // 搜尋中 overlay 被隱藏也不能斷線
    running_ = true;
    // 16KB stack 就夠：所有大緩衝都在全域
    if (R_FAILED(threadCreate(&thread_, threadMain, this, nullptr, 0x4000, 0x2C, -2)) ||
        R_FAILED(threadStart(&thread_))) {
        running_ = false;
        session().setKeepAlive(false);
        return false;
    }
    threadValid_ = true;
    return true;
}

void SearchTask::threadMain(void *arg) {
    auto *self = static_cast<SearchTask *>(arg);
    DmntReader reader(self->scope_);

    // 暫停遊戲，數值才不會在搜尋途中變動；搜完一定要恢復
    dmntchtPauseCheatProcess();
    self->last_ = self->first_
        ? g_engine.firstScan(reader, self->type_, self->cond_, self->value_, self->progress_)
        : g_engine.nextScan(reader, self->cond_, self->value_, self->progress_);
    dmntchtResumeCheatProcess();

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
