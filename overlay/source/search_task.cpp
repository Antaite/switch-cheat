#include "search_task.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include "cheat_session.hpp"
#include "dmntcht.h"

namespace cheattw {

namespace {

// 引擎約 230KB，一定要放全域，不能放 stack
SearchEngine g_engine;

// 診斷資訊：寫進 search.log
char g_diag[512];

class DmntReader : public MemoryReader {
public:
    explicit DmntReader(SearchScope s) : scope_(s) {}

    size_t regions(Region *out, size_t max) override {
        const auto &e = session().extents();
        g_diag[0] = '\0';
        // 方法一（EdiZon 用的）：跟 dmnt 要整份記憶體對照表，再挑出 heap／主程式範圍內可讀寫的
        size_t n = fromMappings(e, out, max);
        if (n > 0) return n;
        // 方法二（備援）：從 heap 起點一段一段查詢
        if (scope_ == SearchScope::WithMain || scope_ == SearchScope::MainOnly) n = fromQuery(e.mainBase, e.mainSize, out, n, max);
        if (scope_ == SearchScope::MainOnly) return n;
        n = fromQuery(e.heapBase, e.heapSize, out, n, max);
        n = fromQuery(e.aliasBase, e.aliasSize, out, n, max);
        return n;
    }

    bool read(uint64_t addr, void *buf, size_t size) override {
        return R_SUCCEEDED(dmntchtReadCheatProcessMemory(addr, buf, size));
    }

private:
    static bool usable(const MemoryInfo &mi) {
        return (mi.perm & Perm_Rw) == Perm_Rw && mi.type != MemType_Unmapped &&
               mi.type != MemType_Io && mi.type != MemType_Reserved;
    }

    static bool within(u64 addr, u64 base, u64 size) { return size && addr >= base && addr < base + size; }

    bool inScope(const Extents &e, u64 addr) const {
        if (scope_ == SearchScope::All) return true;
        if (scope_ == SearchScope::MainOnly) return within(addr, e.mainBase, e.mainSize);
        if (within(addr, e.heapBase, e.heapSize) || within(addr, e.aliasBase, e.aliasSize)) return true;
        return scope_ == SearchScope::WithMain && within(addr, e.mainBase, e.mainSize);
    }

    static void add(Region *out, size_t &n, size_t max, u64 base, u64 size) {
        if (n > 0 && out[n - 1].base + out[n - 1].size == base) { out[n - 1].size += size; return; }   // 相鄰的合併
        if (n < max) out[n++] = {base, size};
    }

    size_t fromMappings(const Extents &e, Region *out, size_t max) {
        u64 total = 0;
        Result rc = dmntchtGetCheatProcessMappingCount(&total);
        size_t n = 0, kept = 0, rw = 0, inHeap = 0, inAlias = 0;
        static MemoryInfo batch[32];   // 一次拿 32 筆（1.25KB），放全域不佔 stack
        u64 offset = 0;
        while (R_SUCCEEDED(rc) && offset < total) {
            u64 got = 0;
            rc = dmntchtGetCheatProcessMappings(batch, 32, offset, &got);
            if (R_FAILED(rc) || got == 0) break;
            for (u64 i = 0; i < got; ++i) {
                const MemoryInfo &mi = batch[i];
                if (usable(mi)) {
                    ++rw;
                    if (within(mi.addr, e.heapBase, e.heapSize)) ++inHeap;
                    if (within(mi.addr, e.aliasBase, e.aliasSize)) ++inAlias;
                }
                if (!usable(mi) || !inScope(e, mi.addr)) continue;
                add(out, n, max, mi.addr, mi.size);
                ++kept;
            }
            offset += got;
        }
        std::snprintf(g_diag, sizeof(g_diag), "共 %llu 段，可寫 %zu（heap %zu、alias %zu），採用 %zu rc=%x",
                      (unsigned long long)total, rw, inHeap, inAlias, kept, rc);
        return n;
    }

    // 只收可讀寫、有對應實體記憶體的區段
    static size_t fromQuery(u64 base, u64 size, Region *out, size_t n, size_t max) {
        const u64 end = base + size;
        u64 addr = base;
        int logged = 0;
        while (addr < end && n < max) {
            MemoryInfo mi{};
            const Result rc = dmntchtQueryCheatProcessMemory(&mi, addr);
            if (logged < 3) {   // 前三筆寫進診斷，方便看是查詢失敗還是被篩掉
                const size_t len = std::strlen(g_diag);
                std::snprintf(g_diag + len, sizeof(g_diag) - len, " | query@%llx rc=%x addr=%llx size=%llx type=%x perm=%x",
                              (unsigned long long)addr, rc, (unsigned long long)mi.addr, (unsigned long long)mi.size, mi.type, mi.perm);
                ++logged;
            }
            if (R_FAILED(rc) || mi.size == 0) break;
            const u64 rs = std::max<u64>(mi.addr, base);
            const u64 re = std::min<u64>(mi.addr + mi.size, end);
            if (usable(mi) && re > rs) add(out, n, max, rs, re - rs);
            const u64 next = mi.addr + mi.size;
            if (next <= addr) break;   // 防止無窮迴圈
            addr = next;
        }
        return n;
    }

    SearchScope scope_;
};

} // namespace

// 用某個範圍把區段整理成 vector（移植用）
static std::vector<Region> collectRegions(SearchScope scope) {
    static Region tmp[SearchEngine::kMaxRegions];
    DmntReader r(scope);
    const size_t n = r.regions(tmp, SearchEngine::kMaxRegions);
    return std::vector<Region>(tmp, tmp + n);
}

namespace {
struct SavedTask {
    uint32_t magic;          // 'CTWT'
    uint32_t version;
    char tid[17];
    char bid[17];
    uint8_t pad[6];
    u64 pid;
    SearchSettings ui;
};
constexpr uint32_t kTaskMagic = 0x54575443, kTaskVersion = 2;
std::string taskPath()   { return std::string(kSearchDir) + "/task.bin"; }
std::string enginePath() { return std::string(kSearchDir) + "/engine.bin"; }
std::string ptrInfoPath() { return std::string(kSearchDir) + "/ptr.info"; }

// 指標結果屬於哪個遊戲版本、什麼型別
struct PtrInfo { char tid[17]; char bid[17]; uint8_t type; uint8_t pad[5]; uint32_t scale; uint32_t pad2; };
bool readPtrInfo(PtrInfo &pi) {
    FILE *f = std::fopen(ptrInfoPath().c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fread(&pi, sizeof(pi), 1, f) == 1;
    std::fclose(f);
    pi.tid[16] = pi.bid[16] = '\0';
    return ok && session().tid() == pi.tid && session().bid() == pi.bid;
}
}

SearchTask &searchTask() {
    static SearchTask t;
    return t;
}

SearchEngine &SearchTask::engine() { return g_engine; }

// ───────────────────────── 存檔 ─────────────────────────

void SearchTask::persist(const SearchSettings &ui) {
    if (running_ || !g_engine.started()) return;
    mkdirs(kSearchDir);
    g_engine.setWorkDir(kSearchDir);
    if (!g_engine.saveState(enginePath())) return;
    SavedTask t{};
    t.magic = kTaskMagic;
    t.version = kTaskVersion;
    std::snprintf(t.tid, sizeof(t.tid), "%s", session().tid().c_str());
    std::snprintf(t.bid, sizeof(t.bid), "%s", session().bid().c_str());
    t.pid = session().processId();
    t.ui = ui;
    if (FILE *f = std::fopen(taskPath().c_str(), "wb")) {
        std::fwrite(&t, sizeof(t), 1, f);
        std::fclose(f);
    }
}

RestoreResult SearchTask::restore(SearchSettings &ui) {
    if (running_ || g_engine.started() || !session().hasGame()) return RestoreResult::None;
    SavedTask t{};
    FILE *f = std::fopen(taskPath().c_str(), "rb");
    if (!f) return RestoreResult::None;
    const bool ok = std::fread(&t, sizeof(t), 1, f) == 1;
    std::fclose(f);
    if (!ok || t.magic != kTaskMagic || t.version != kTaskVersion) return RestoreResult::None;
    t.tid[16] = t.bid[16] = '\0';
    // 換了遊戲或版本：舊結果沒有意義
    if (session().tid() != t.tid || session().bid() != t.bid) return RestoreResult::None;
    g_engine.setWorkDir(kSearchDir);
    if (!g_engine.loadState(enginePath())) return RestoreResult::None;
    t.ui.value[31] = t.ui.value2[31] = t.ui.value3[31] = '\0';
    ui = t.ui;
    scope_ = ui.scope;
    // 遊戲重開過（程序編號不同），位址多半已經變了
    return t.pid == session().processId() ? RestoreResult::Restored : RestoreResult::RestoredStale;
}

void SearchTask::clearSaved() {
    std::remove(taskPath().c_str());
    std::remove(enginePath().c_str());
}

void SearchTask::reset() {
    if (running_) return;
    mkdirs(kSearchDir);
    g_engine.setWorkDir(kSearchDir);
    g_engine.reset();
    clearSaved();
    last_ = SearchResult::NotStarted;
}

// ───────────────────────── 啟動工作 ─────────────────────────

bool SearchTask::launch(JobKind job, std::string *why) {
    auto fail = [why](const char *m) { if (why) *why = m; return false; };
    if (running_) return fail("已經有工作在跑了");
    if (!session().isOpen()) return fail("dmnt 沒有連線，請回首頁按「重新連線」");
    if (!session().hasGame()) return fail("沒有執行中的遊戲");
    if (threadValid_) { threadWaitForExit(&thread_); threadClose(&thread_); threadValid_ = false; }

    mkdirs(kSearchDir);
    g_engine.setWorkDir(kSearchDir);
    job_ = job;
    progress_.cancel = false;
    progress_.done = 0;
    progress_.total = 0;
    summary_[0] = '\0';
    session().setKeepAlive(true);   // 工作中 overlay 被隱藏也不能斷線
    running_ = true;
    // 大緩衝都在全域或 heap，64KB stack 很充裕
    if (R_FAILED(threadCreate(&thread_, threadMain, this, nullptr, 0x10000, 0x2C, -2)) ||
        R_FAILED(threadStart(&thread_))) {
        running_ = false;
        session().setKeepAlive(false);
        return fail("無法建立背景工作（記憶體不足）");
    }
    threadValid_ = true;
    return true;
}

bool SearchTask::startScan(bool first, const ScanSpec &spec, SearchScope scope, std::string *why) {
    if (session().hasGame() && session().extents().heapSize == 0 && session().extents().aliasSize == 0) {
        if (why) *why = "讀不到遊戲的記憶體範圍";
        return false;
    }
    first_ = first;
    spec_ = spec;
    scope_ = scope;
    return launch(JobKind::Scan, why);
}

bool SearchTask::startPointerSearch(u64 target, const Encoding &enc, std::string *why) {
    ptrTarget_ = target;
    ptrEnc_ = enc;
    return launch(JobKind::PointerSearch, why);
}

bool SearchTask::startPointerFilter(u64 newTarget, std::string *why) {
    if (pointerCount() == 0) { if (why) *why = "還沒有指標搜尋結果"; return false; }
    ptrTarget_ = newTarget;
    return launch(JobKind::PointerFilter, why);
}

bool SearchTask::startPort(std::string *why) {
    return launch(JobKind::Port, why);
}

std::string SearchTask::pointerPath() const { return std::string(kSearchDir) + "/ptr.bin"; }

size_t SearchTask::pointerCount() const {
    PtrInfo pi;
    if (!readPtrInfo(pi)) return 0;
    FILE *f = std::fopen(pointerPath().c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n > 0 ? (size_t)n / sizeof(PtrChain) : 0;
}

Encoding SearchTask::pointerEncoding() const {
    PtrInfo pi;
    if (!readPtrInfo(pi) || pi.type > (uint8_t)ValueType::F64) return {ValueType::U32, 1};
    return {(ValueType)pi.type, pi.scale ? pi.scale : 1};
}

// ───────────────────────── 執行 ─────────────────────────

void SearchTask::threadMain(void *arg) {
    auto *self = static_cast<SearchTask *>(arg);
    const u64 t0 = armTicksToNs(armGetSystemTick());
    // 暫停遊戲，數值才不會在途中變動；做完一定要恢復
    dmntchtPauseCheatProcess();
    switch (self->job_) {
        case JobKind::Scan:          self->runScan(); break;
        case JobKind::PointerSearch: self->runPointerSearch(); break;
        case JobKind::PointerFilter: self->runPointerFilter(); break;
        case JobKind::Port:          self->runPort(); break;
        default: break;
    }
    dmntchtResumeCheatProcess();

    const u64 sec = (armTicksToNs(armGetSystemTick()) - t0) / 1000000000ull;
    const size_t len = std::strlen(self->summary_);
    std::snprintf(self->summary_ + len, sizeof(self->summary_) - len, "%s花 %llu 秒", len ? "，" : "", (unsigned long long)sec);
    if (FILE *f = std::fopen((std::string(kAppDir) + "/search.log").c_str(), "a")) {
        const auto &e = session().extents();
        std::fprintf(f, "工作=%d 結果=%s %s heap=%llx+%llx alias=%llx+%llx main=%llx+%llx\n  診斷：%s\n",
                     (int)self->job_, searchResultText(self->last_), self->summary_,
                     (unsigned long long)e.heapBase, (unsigned long long)e.heapSize,
                     (unsigned long long)e.aliasBase, (unsigned long long)e.aliasSize,
                     (unsigned long long)e.mainBase, (unsigned long long)e.mainSize, g_diag);
        std::fclose(f);
    }
    session().setKeepAlive(false);
    self->running_ = false;
    self->finished_ = true;
}

void SearchTask::runScan() {
    DmntReader reader(scope_);
    last_ = first_ ? g_engine.firstScan(reader, spec_, progress_)
                   : g_engine.nextScan(reader, spec_.cond, spec_.value, progress_);
    if (last_ == SearchResult::NoRegions) std::snprintf(summary_, sizeof(summary_), "%s", g_diag);
    else if (first_ && g_engine.inSnapshot())
        std::snprintf(summary_, sizeof(summary_), "記下 %u MB", g_engine.snapshotMB());
    else if (first_)
        std::snprintf(summary_, sizeof(summary_), "掃描 %llu MB", (unsigned long long)(progress_.total.load() >> 20));
}

void SearchTask::runPointerSearch() {
    const auto &e = session().extents();
    DmntReader reader(SearchScope::WithMain);
    size_t n = 0;
    last_ = PointerSearch::search(reader, ptrTarget_, Region{e.mainBase, e.mainSize}, PtrConfig{}, progress_,
                                  pointerPath(), n);
    if (last_ == SearchResult::Ok) {
        PtrInfo pi{};
        std::snprintf(pi.tid, sizeof(pi.tid), "%s", session().tid().c_str());
        std::snprintf(pi.bid, sizeof(pi.bid), "%s", session().bid().c_str());
        pi.type = (uint8_t)ptrEnc_.type;
        pi.scale = ptrEnc_.scale;
        if (FILE *f = std::fopen(ptrInfoPath().c_str(), "wb")) { std::fwrite(&pi, sizeof(pi), 1, f); std::fclose(f); }
    }
    std::snprintf(summary_, sizeof(summary_), "找到 %zu 條指標路徑", n);
}

void SearchTask::runPointerFilter() {
    DmntReader reader(SearchScope::WithMain);
    size_t n = 0;
    last_ = PointerSearch::filter(reader, session().extents().mainBase, ptrTarget_, pointerPath(), pointerPath(), n, progress_);
    std::snprintf(summary_, sizeof(summary_), "剩 %zu 條", n);
}

void SearchTask::runPort() {
    portResults_.clear();
    // 收集 db/{TID}/other/ 裡所有其他版本的碼（同名同內容只留一條）
    std::vector<Cheat> cheats;
    const std::string dir = std::string(kDbDir) + "/" + session().tid() + "/other";
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *ent = readdir(d)) {
            std::string n = ent->d_name;
            if (n.size() < 4 || n.compare(n.size() - 4, 4, ".txt") != 0) continue;
            CheatFile f = parseCheatFile(dir + "/" + n);
            for (auto &c : f.cheats) {
                if (c.isMaster) continue;
                bool dup = false;
                for (auto &o : cheats) if (o.name == c.name && o.opcodes == c.opcodes) { dup = true; break; }
                if (!dup) cheats.push_back(c);
            }
        }
        closedir(d);
    }
    if (cheats.empty()) {
        last_ = SearchResult::NotStarted;
        std::snprintf(summary_, sizeof(summary_), "沒有其他版本的碼可以移植");
        return;
    }
    const auto mainRw = collectRegions(SearchScope::MainOnly);
    const auto valid = collectRegions(SearchScope::All);
    if (mainRw.empty() || valid.empty()) { last_ = SearchResult::NoRegions; return; }
    DmntReader reader(SearchScope::All);
    portResults_ = Porter::portAll(reader, session().extents().mainBase, mainRw, valid, cheats, 16ull << 20, progress_);
    last_ = progress_.cancel ? SearchResult::Cancelled : SearchResult::Ok;
    size_t portable = 0, ok = 0, high = 0;
    for (auto &r : portResults_) { portable += r.portable; ok += r.ok; high += r.ok && r.votes >= 2; }
    std::snprintf(summary_, sizeof(summary_), "%zu 條碼中 %zu 條是指標碼，成功 %zu 條（信心高 %zu）",
                  cheats.size(), portable, ok, high);
}

bool SearchTask::locate(u64 addr, u8 &regionType, u64 &offset, std::string &label) const {
    const auto &e = session().extents();
    char buf[32];
    if (addr >= e.heapBase && addr < e.heapBase + e.heapSize) {
        regionType = 1; offset = addr - e.heapBase;
        std::snprintf(buf, sizeof(buf), "H+%llX", (unsigned long long)offset);
    } else if (addr >= e.mainBase && addr < e.mainBase + e.mainSize) {
        regionType = 0; offset = addr - e.mainBase;
        std::snprintf(buf, sizeof(buf), "M+%llX", (unsigned long long)offset);
    } else if (e.aliasSize && addr >= e.aliasBase && addr < e.aliasBase + e.aliasSize) {
        regionType = 2; offset = addr - e.aliasBase;
        std::snprintf(buf, sizeof(buf), "A+%llX", (unsigned long long)offset);
    } else if (e.spaceSize && addr >= e.spaceBase && addr < e.spaceBase + e.spaceSize) {
        regionType = 3; offset = addr - e.spaceBase;
        std::snprintf(buf, sizeof(buf), "R+%llX", (unsigned long long)offset);
    } else {
        return false;
    }
    label = buf;
    return true;
}

} // namespace cheattw
