// 指標搜尋測試：g++ -std=c++17 -I../include test_pointer.cpp ../source/pointer_search.cpp ../source/search_engine.cpp ../source/cheat_core.cpp
#include "pointer_search.hpp"
#include <cassert>
#include <cstring>
#include <map>
#include <vector>

using namespace cheattw;

struct FakeMemory : MemoryReader {
    std::map<uint64_t, std::vector<uint8_t>> segs;
    size_t regions(Region *out, size_t max) override {
        size_t n = 0;
        for (auto &s : segs) if (n < max) out[n++] = {s.first, s.second.size()};
        return n;
    }
    bool read(uint64_t addr, void *buf, size_t size) override {
        for (auto &s : segs)
            if (addr >= s.first && addr + size <= s.first + s.second.size()) {
                std::memcpy(buf, s.second.data() + (addr - s.first), size);
                return true;
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

// 遊戲的結構：主程式 +0x2000 存「遊戲管理器」指標 → 管理器 +0x30 存「玩家」指標 → 玩家 +0x18 是金錢
static void layout(FakeMemory &m, uint64_t mainBase, uint64_t mgr, uint64_t player, uint32_t money) {
    m.clear();
    m.put<uint64_t>(mainBase + 0x2000, mgr);
    m.put<uint64_t>(mgr + 0x30, player);
    m.put<uint32_t>(player + 0x18, money);
    // 干擾：heap 裡也有幾個指向玩家的指標，但它們本身不在主程式裡、也沒人指
    m.put<uint64_t>(0x80001000, player);
    m.put<uint64_t>(0x80002000, player + 0x10);
}

int main() {
    const uint64_t mainBase = 0x10000000;
    FakeMemory mem;
    mem.segs[mainBase] = std::vector<uint8_t>(0x10000, 0);     // 主程式
    mem.segs[0x80000000] = std::vector<uint8_t>(0x40000, 0);   // 遊戲資料區
    const Region mainR{mainBase, 0x10000};

    layout(mem, mainBase, 0x80010000, 0x80020000, 460);
    const uint64_t money = 0x80020000 + 0x18;

    SearchProgress p;
    size_t n = 0;
    assert(PointerSearch::search(mem, money, mainR, PtrConfig{}, p, "/tmp/ptr.bin", n) == SearchResult::Ok);
    assert(n >= 1);
    PtrChain chains[16];
    const size_t got = PointerSearch::read("/tmp/ptr.bin", chains, 16, 0);
    bool found = false;
    for (size_t i = 0; i < got; ++i) {
        std::printf("  %s\n", PointerSearch::describe(chains[i]).c_str());
        if (chains[i].depth == 2 && chains[i].baseOff == 0x2000 && chains[i].offs[0] == 0x30 && chains[i].offs[1] == 0x18) found = true;
        uint64_t a;
        assert(PointerSearch::resolve(mem, mainBase, chains[i], a) && a == money);
    }
    assert(found);

    // 「重開遊戲」：管理器和玩家換到別的位址，只有主程式裡的起點不變
    layout(mem, mainBase, 0x80030000, 0x80038000, 460);
    const uint64_t money2 = 0x80038000 + 0x18;
    size_t kept = 0;
    assert(PointerSearch::filter(mem, mainBase, money2, "/tmp/ptr.bin", "/tmp/ptr.bin", kept, p) == SearchResult::Ok);
    assert(kept >= 1);
    assert(PointerSearch::read("/tmp/ptr.bin", chains, 16, 0) == kept);
    for (size_t i = 0; i < kept; ++i) {
        uint64_t a;
        assert(PointerSearch::resolve(mem, mainBase, chains[i], a) && a == money2);
    }

    // 產生的金手指碼
    PtrChain c{};
    c.depth = 2; c.baseOff = 0x2000; c.offs[0] = 0x30; c.offs[1] = 0x18;
    Cheat k = PointerSearch::makeCheat("金錢", c, ValueType::U32, 999999);
    const uint32_t expect[] = {0x580F0000, 0x2000, 0x580F1000, 0x30, 0x780F0000, 0x18, 0x640F0000, 0, 999999};
    assert(k.opcodes.size() == 9);
    for (int i = 0; i < 9; ++i) assert(k.opcodes[i] == expect[i]);
    assert(PointerSearch::describe(c) == "M+2000 → +30 → +18");

    std::printf("指標搜尋測試全部通過（找到 %zu 條，重開後剩 %zu 條）\n", n, kept);
    return 0;
}
