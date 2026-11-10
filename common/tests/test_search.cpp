// 搜尋引擎測試（假記憶體）：
// g++ -std=c++17 -I../include test_search.cpp ../source/search_engine.cpp ../source/cheat_core.cpp
#include "search_engine.hpp"
#include <cassert>
#include <cstring>
#include <map>
#include <vector>

using namespace cheattw;

// 三段記憶體：中間有空洞、第二段有一頁讀不到
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
    void put32(uint64_t addr, uint32_t v) {
        for (auto &s : segs)
            if (addr >= s.first && addr + 4 <= s.first + s.second.size()) { std::memcpy(&s.second[addr - s.first], &v, 4); return; }
        assert(!"bad addr");
    }
    void putF(uint64_t addr, float f) { uint32_t b; std::memcpy(&b, &f, 4); put32(addr, b); }
};

static SearchEngine g_engine;   // 引擎約 150KB，實機上也要放全域，不能放 stack

int main() {
    FakeMemory mem;
    mem.segs[0x10000000] = std::vector<uint8_t>(200 * 1024, 0);   // 不是 64KB 的倍數
    mem.segs[0x20000000] = std::vector<uint8_t>(300 * 1024, 0);
    mem.segs[0x30000000] = std::vector<uint8_t>(4096, 0);

    // 血量 850 放在幾個地方，其中一個剛好在 64KB 區塊邊界前
    const uint64_t hp = 0x20000000 + 64 * 1024 - 4;
    mem.put32(0x10000010, 850);
    mem.put32(hp, 850);
    mem.put32(0x20030000, 850);
    mem.put32(0x30000100, 850);

    g_engine.setWorkDir("/tmp");
    g_engine.reset();
    SearchProgress p;

    // 第一次：等於 850
    assert(g_engine.firstScan(mem, ValueType::U32, Cond::Equal, 850, p) == SearchResult::Ok);
    assert(g_engine.count() == 4);
    assert(p.done == p.total);

    // 遊戲裡扣血：只有 hp 變成 820，其他不變
    mem.put32(hp, 820);
    assert(g_engine.nextScan(mem, Cond::Decreased, 0, p) == SearchResult::Ok);
    assert(g_engine.count() == 1);
    Candidate c;
    assert(g_engine.peek(&c, 1, 0) == 1 && c.addr == hp && c.value == 820);

    // 沒變也能篩
    assert(g_engine.nextScan(mem, Cond::Unchanged, 0, p) == SearchResult::Ok);
    assert(g_engine.count() == 1);

    // 讀不到的頁面：候選被丟掉，不會當掉
    g_engine.reset();
    assert(g_engine.firstScan(mem, ValueType::U32, Cond::Equal, 850, p) == SearchResult::Ok);
    assert(g_engine.count() == 3);
    mem.badPage = 0x20030000;
    assert(g_engine.nextScan(mem, Cond::Unchanged, 0, p) == SearchResult::Ok);
    assert(g_engine.count() == 2);
    mem.badPage = 0;

    // 結果太多：上限 1000 筆，搜尋 0（到處都是）要擋下來，而且保留上一次的結果
    g_engine.setMaxCandidates(1000);
    assert(g_engine.firstScan(mem, ValueType::U32, Cond::Equal, 0, p) == SearchResult::TooMany);
    assert(g_engine.count() == 2);
    g_engine.setMaxCandidates(2000000);

    // 取消
    p.cancel = true;
    assert(g_engine.firstScan(mem, ValueType::U32, Cond::Equal, 850, p) == SearchResult::Cancelled);
    assert(g_engine.count() == 2);
    p.cancel = false;

    // 小數：搜尋 1.5 會找到 1.50003（容許誤差）
    g_engine.reset();
    mem.putF(0x10000100, 1.50003f);
    assert(g_engine.firstScan(mem, ValueType::F32, Cond::Equal, encodeValue(ValueType::F32, 1.5), p) == SearchResult::Ok);
    assert(g_engine.count() == 1);
    assert(formatValue(ValueType::F32, encodeValue(ValueType::F32, 1.5)) == "1.5");

    // 第一次搜尋不能用「變大了」
    g_engine.reset();
    assert(g_engine.firstScan(mem, ValueType::U32, Cond::Increased, 0, p) == SearchResult::NotStarted);

    // 讀取次數：第二次搜尋同一個 64KB 視窗內的候選只讀一次
    g_engine.reset();
    for (int i = 0; i < 100; ++i) mem.put32(0x20000000 + 0x100 + i * 4, 777);
    assert(g_engine.firstScan(mem, ValueType::U32, Cond::Equal, 777, p) == SearchResult::Ok);
    assert(g_engine.count() == 100);
    mem.reads = 0;
    assert(g_engine.nextScan(mem, Cond::Unchanged, 0, p) == SearchResult::Ok);
    assert(mem.reads == 1);

    // 存檔後換一個新的引擎讀回來，要能接著搜（模擬 overlay 關掉再打開）
    {
        static SearchEngine fresh;
        fresh.setWorkDir("/tmp");
        assert(g_engine.saveState("/tmp/state.bin"));
        assert(fresh.loadState("/tmp/state.bin"));
        assert(fresh.started() && fresh.count() == 100 && fresh.type() == ValueType::U32);
        mem.put32(0x20000000 + 0x100, 778);
        assert(fresh.nextScan(mem, Cond::Increased, 0, p) == SearchResult::Ok);
        assert(fresh.count() == 1);
        // 候選檔被刪掉或大小對不上，就不能讀回來
        fresh.reset();
        assert(!fresh.loadState("/tmp/state.bin"));
    }

    // 產生金手指碼：heap + 0x01A3F2C8 寫入 u32 999
    Cheat k = makeStaticWriteCheat("血量鎖定", 1, 0x01A3F2C8, ValueType::U32, 999);
    assert(k.opcodes.size() == 3 && k.opcodes[0] == 0x04100000 && k.opcodes[1] == 0x01A3F2C8 && k.opcodes[2] == 999);
    Cheat k8 = makeStaticWriteCheat("x", 0, 0x123456789Aull, ValueType::U64, 0x1122334455667788ull);
    assert(k8.opcodes.size() == 4 && k8.opcodes[0] == 0x08000012 && k8.opcodes[1] == 0x3456789A &&
           k8.opcodes[2] == 0x11223344 && k8.opcodes[3] == 0x55667788);

    std::printf("搜尋引擎測試全部通過（引擎物件 %zu KB）\n", sizeof(SearchEngine) / 1024);
    return 0;
}
