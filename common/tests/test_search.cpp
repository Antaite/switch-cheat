// 搜尋引擎測試（假記憶體）：
// g++ -std=c++17 -I../include test_search.cpp ../source/search_engine.cpp ../source/cheat_core.cpp
#include "search_engine.hpp"
#include <cassert>
#include <cstring>
#include <map>
#include <vector>

using namespace cheattw;

// 三段記憶體：中間有空洞、可以指定一頁讀不到
struct FakeMemory : MemoryReader {
    std::map<uint64_t, std::vector<uint8_t>> segs;
    uint64_t badPage = 0;
    size_t reads = 0;

    size_t regions(Region *out, size_t max) override {
        size_t n = 0;
        for (auto &s : segs) if (n < max) out[n++] = {s.first, s.second.size()};
        return n;
    }
    bool read(uint64_t addr, void *buf, size_t size) override {
        ++reads;
        if (badPage && addr <= badPage && badPage < addr + size) return false;
        for (auto &s : segs) {
            if (addr >= s.first && addr + size <= s.first + s.second.size()) {
                std::memcpy(buf, s.second.data() + (addr - s.first), size);
                return true;
            }
        }
        return false;
    }
    template <typename T> void put(uint64_t addr, T v) {
        for (auto &s : segs)
            if (addr >= s.first && addr + sizeof(T) <= s.first + s.second.size()) { std::memcpy(&s.second[addr - s.first], &v, sizeof(T)); return; }
        assert(!"bad addr");
    }
    void clear() { for (auto &s : segs) std::fill(s.second.begin(), s.second.end(), 0); }
};

static SearchEngine g_engine;   // 引擎約 230KB，實機上也要放全域，不能放 stack
static uint64_t bigDisk(const std::string &) { return 64ull << 30; }
static uint64_t tinyDisk(const std::string &) { return 1 << 20; }

static ScanSpec normal(ValueType t, double v) { ScanSpec s; s.type = t; s.value = v; return s; }

int main() {
    FakeMemory mem;
    mem.segs[0x10000000] = std::vector<uint8_t>(200 * 1024, 0);   // 不是 64KB 的倍數
    mem.segs[0x20000000] = std::vector<uint8_t>(300 * 1024, 0);
    mem.segs[0x30000000] = std::vector<uint8_t>(4096, 0);
    g_engine.setWorkDir("/tmp");
    g_engine.setFreeSpaceFn(bigDisk);
    SearchProgress p;

    // ── 一般搜尋 ──
    const uint64_t hp = 0x20000000 + 64 * 1024 - 4;   // 剛好在 64KB 區塊邊界前
    mem.put<uint32_t>(0x10000010, 850);
    mem.put<uint32_t>(hp, 850);
    mem.put<uint32_t>(0x20030000, 850);
    mem.put<uint32_t>(0x30000100, 850);
    g_engine.reset();
    assert(g_engine.firstScan(mem, normal(ValueType::U32, 850), p) == SearchResult::Ok);
    assert(g_engine.count() == 4 && p.done == p.total);
    mem.put<uint32_t>(hp, 820);
    assert(g_engine.nextScan(mem, Cond::Decreased, 0, p) == SearchResult::Ok);
    assert(g_engine.count() == 1);
    Candidate c;
    assert(g_engine.peek(&c, 1, 0) == 1 && candAddr(c) == hp && c.value == 820);

    // 讀不到的頁面：候選被丟掉，不會當掉
    mem.put<uint32_t>(hp, 850);
    assert(g_engine.firstScan(mem, normal(ValueType::U32, 850), p) == SearchResult::Ok);
    mem.badPage = 0x20030000;
    assert(g_engine.nextScan(mem, Cond::Unchanged, 0, p) == SearchResult::Ok);
    assert(g_engine.count() == 3);
    mem.badPage = 0;

    // 結果太多要擋下來，而且保留上一次的結果
    g_engine.setMaxCandidates(1000);
    assert(g_engine.firstScan(mem, normal(ValueType::U32, 0), p) == SearchResult::TooMany);
    assert(g_engine.count() == 3 && g_engine.mode() == SearchMode::Normal);
    g_engine.setMaxCandidates(2000000);

    // 取消
    p.cancel = true;
    assert(g_engine.firstScan(mem, normal(ValueType::U32, 850), p) == SearchResult::Cancelled);
    assert(g_engine.count() == 3);
    p.cancel = false;

    // 第一次搜尋不能用「變大了」
    ScanSpec inc = normal(ValueType::U32, 0);
    inc.cond = Cond::Increased;
    g_engine.reset();
    assert(g_engine.firstScan(mem, inc, p) == SearchResult::NotStarted);

    // 同一個 64KB 視窗內的候選只讀一次
    mem.clear();
    for (int i = 0; i < 100; ++i) mem.put<uint32_t>(0x20000000 + 0x100 + i * 4, 777);
    assert(g_engine.firstScan(mem, normal(ValueType::U32, 777), p) == SearchResult::Ok);
    assert(g_engine.count() == 100);
    mem.reads = 0;
    assert(g_engine.nextScan(mem, Cond::Unchanged, 0, p) == SearchResult::Ok);
    assert(mem.reads == 1);

    // ── 智慧搜尋：同一個畫面數值 460 用各種存法藏在不同地方 ──
    mem.clear();
    mem.put<uint32_t>(0x10000100, 460);         // 整數
    mem.put<float>(0x10000200, 460.0f);          // float
    mem.put<uint32_t>(0x10000300, 4600);         // ×10
    mem.put<uint32_t>(0x10000400, 46000);        // ×100
    mem.put<double>(0x10000500, 460.0);          // double
    mem.put<uint16_t>(0x10000602, 460);          // 2 bytes（≥256 才會試）
    ScanSpec smart;
    smart.mode = SearchMode::Smart;
    smart.value = 460;
    assert(g_engine.firstScan(mem, smart, p) == SearchResult::Ok);
    assert(g_engine.count() == 6);
    Candidate all[8];
    assert(g_engine.peek(all, 8, 0) == 6);
    std::map<uint64_t, std::string> seen;
    for (int i = 0; i < 6; ++i) seen[candAddr(all[i])] = encodingName(g_engine.encoding(candEnc(all[i])));
    assert(seen[0x10000100] == "整數4" && seen[0x10000200] == "float" && seen[0x10000300] == "整數4×10" &&
           seen[0x10000400] == "整數4×100" && seen[0x10000500] == "double" && seen[0x10000602] == "整數2");
    assert(formatEncoded(g_engine.encoding(candEnc(all[2])), all[2].value) == "460");
    // 接著搜：畫面變成 480，每個候選用自己的存法比
    mem.put<uint32_t>(0x10000300, 4800);
    mem.put<float>(0x10000200, 480.0f);
    assert(g_engine.nextScan(mem, Cond::Equal, 480, p) == SearchResult::Ok);
    assert(g_engine.count() == 2);

    // ── 群組搜尋：血量 120、最大血量 150 靠在一起；到處都有零散的 120 ──
    mem.clear();
    for (int i = 0; i < 50; ++i) mem.put<uint32_t>(0x20000000 + 0x1000 + i * 0x400, 120);
    mem.put<uint32_t>(0x20020000, 120);
    mem.put<uint32_t>(0x20020008, 150);
    // 跨 64KB 區塊邊界的一組：120 在區塊尾、150 在下一塊開頭
    mem.put<uint32_t>(0x20000000 + 0x10000 - 4, 120);
    mem.put<uint32_t>(0x20000000 + 0x10000 + 8, 150);
    ScanSpec grp;
    grp.mode = SearchMode::Group;
    grp.type = ValueType::U32;
    grp.value = 120;
    grp.extra[0] = 150;
    grp.extraCount = 1;
    grp.window = 64;
    assert(g_engine.firstScan(mem, grp, p) == SearchResult::Ok);
    assert(g_engine.count() == 2);
    assert(g_engine.peek(all, 2, 0) == 2);
    int32_t offs[2];
    assert(g_engine.groupNeighbors(mem, candAddr(all[1]), offs) == 1 && offs[0] == 8);
    ScanSpec badGrp = grp;
    badGrp.extraCount = 0;
    assert(g_engine.firstScan(mem, badGrp, p) == SearchResult::BadSpec);

    // ── 未知數值：血條 float 從 1.0 變 0.8 再變 0.6 ──
    mem.clear();
    for (int i = 0; i < 2000; ++i) mem.put<float>(0x10000000 + i * 4, (float)(i % 7));   // 一堆不會動的雜訊
    const uint64_t bar = 0x20010040;
    mem.put<float>(bar, 1.0f);
    mem.put<uint32_t>(0x20000800, 5);   // 一個會一直變大的計數器（要被「變小了」排除）
    ScanSpec unk;
    unk.mode = SearchMode::Unknown;
    unk.type = ValueType::F32;
    g_engine.setFreeSpaceFn(tinyDisk);
    assert(g_engine.firstScan(mem, unk, p) == SearchResult::NoSpace);
    g_engine.setFreeSpaceFn(bigDisk);
    assert(g_engine.firstScan(mem, unk, p) == SearchResult::Ok);
    assert(g_engine.inSnapshot() && g_engine.count() == (200 + 300 + 4) * 1024 / 4);
    // 上限設很小，模擬「還太多」→ 留在快照階段
    g_engine.setMaxCandidates(10);
    assert(g_engine.nextScan(mem, Cond::Unchanged, 0, p) == SearchResult::Ok);
    assert(g_engine.inSnapshot());
    // 存檔／讀檔也要能接回快照階段
    assert(g_engine.saveState("/tmp/state.bin"));
    {
        static SearchEngine fresh;
        fresh.setWorkDir("/tmp");
        assert(fresh.loadState("/tmp/state.bin") && fresh.inSnapshot() && fresh.count() == g_engine.count());
    }
    mem.put<float>(bar, 0.8f);
    mem.put<uint32_t>(0x20000800, 9);
    assert(g_engine.nextScan(mem, Cond::Decreased, 0, p) == SearchResult::Ok);
    // 只剩血條：少於上限，轉成一般候選清單
    assert(!g_engine.inSnapshot() && g_engine.count() == 1);
    assert(g_engine.peek(&c, 1, 0) == 1 && candAddr(c) == bar);
    mem.put<float>(bar, 0.6f);
    assert(g_engine.nextScan(mem, Cond::Decreased, 0, p) == SearchResult::Ok && g_engine.count() == 1);
    g_engine.setMaxCandidates(2000000);

    // ── 存檔後換一個新的引擎讀回來，要能接著搜（模擬 overlay 關掉再打開） ──
    mem.clear();
    for (int i = 0; i < 100; ++i) mem.put<uint32_t>(0x20000000 + 0x100 + i * 4, 777);
    assert(g_engine.firstScan(mem, normal(ValueType::U32, 777), p) == SearchResult::Ok);
    {
        static SearchEngine fresh;
        fresh.setWorkDir("/tmp");
        assert(g_engine.saveState("/tmp/state.bin"));
        assert(fresh.loadState("/tmp/state.bin"));
        assert(fresh.started() && fresh.count() == 100 && fresh.type() == ValueType::U32);
        mem.put<uint32_t>(0x20000000 + 0x100, 778);
        assert(fresh.nextScan(mem, Cond::Increased, 0, p) == SearchResult::Ok);
        assert(fresh.count() == 1);
        fresh.reset();   // 候選檔被刪掉，就不能讀回來
        assert(!fresh.loadState("/tmp/state.bin"));
    }

    // ── 產生金手指碼 ──
    Cheat k = makeStaticWriteCheat("血量鎖定", 1, 0x01A3F2C8, ValueType::U32, 999);
    assert(k.opcodes.size() == 3 && k.opcodes[0] == 0x04100000 && k.opcodes[1] == 0x01A3F2C8 && k.opcodes[2] == 999);
    Cheat k8 = makeStaticWriteCheat("x", 0, 0x123456789Aull, ValueType::U64, 0x1122334455667788ull);
    assert(k8.opcodes.size() == 4 && k8.opcodes[0] == 0x08000012 && k8.opcodes[1] == 0x3456789A &&
           k8.opcodes[2] == 0x11223344 && k8.opcodes[3] == 0x55667788);
    assert(encodeDisplay({ValueType::U32, 10}, 999) == 9990);

    std::printf("搜尋引擎測試全部通過（引擎物件 %zu KB）\n", sizeof(SearchEngine) / 1024);
    return 0;
}
