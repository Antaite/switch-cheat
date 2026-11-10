// 跨版本移植測試：g++ -std=c++17 -I../include test_porter.cpp ../source/porter.cpp ../source/search_engine.cpp ../source/cheat_core.cpp
#include "porter.hpp"
#include <cassert>
#include <cstring>
#include <map>

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
};

static Cheat code(const char *name, uint32_t base, uint32_t deref, uint32_t add) {
    Cheat c;
    c.name = name;
    c.opcodes = {0x580F0000, base, 0x580F1000, deref, 0x780F0000, add, 0x640F0000, 0, 999};
    return c;
}

int main() {
    const uint64_t mainBase = 0x10000000;
    FakeMemory mem;
    mem.segs[mainBase] = std::vector<uint8_t>(0x100000, 0);
    mem.segs[0x80000000] = std::vector<uint8_t>(0x40000, 0);

    // 新版本：所有靜態變數往後移了 0x400
    mem.put<uint64_t>(mainBase + 0x2400, 0x80010000);    // 遊戲管理器
    mem.put<uint64_t>(0x80010000 + 0x30, 0x80020000);    // → 玩家
    mem.put<uint64_t>(mainBase + 0x2408, 0x80011000);    // 背包
    mem.put<uint64_t>(0x80011000 + 0x10, 0x80021000);    // → 道具表
    // 干擾：另一個位置剛好也能照「+0x30」走得通（位移 +0x1000）
    mem.put<uint64_t>(mainBase + 0x3000, 0x80012000);
    mem.put<uint64_t>(0x80012000 + 0x30, 0x80022000);

    // 舊版本的碼（起點在 0x2000、0x2008）＋一條不是指標碼的
    std::vector<Cheat> old = {
        code("Infinite HP", 0x2000, 0x30, 0x18),
        code("Max Items", 0x2008, 0x10, 0x4),
    };
    Cheat stat;
    stat.name = "Static";
    stat.opcodes = {0x04000000, 0x1234, 99};
    old.push_back(stat);

    uint64_t base;
    uint8_t reg;
    std::vector<uint64_t> d;
    assert(Porter::parse(old[0], base, reg, d) && base == 0x2000 && reg == 0xF && d.size() == 1 && d[0] == 0x30);
    assert(!Porter::parse(stat, base, reg, d));

    const std::vector<Region> mainRw = {{mainBase, 0x100000}};
    const std::vector<Region> valid = {{mainBase, 0x100000}, {0x80000000, 0x40000}};
    SearchProgress p;
    auto res = Porter::portAll(mem, mainBase, mainRw, valid, old, 0x100000, p);
    assert(res.size() == 3);
    for (auto &r : res)
        std::printf("  %-12s %s 位移 %+lld 票數 %d\n", r.name.c_str(), !r.portable ? "不是指標碼" : r.ok ? "成功" : "失敗",
                    (long long)r.delta, r.votes);

    // 兩條碼都選 +0x400，而且有 2 票（信心高）；干擾的 +0x1000 只有 1 票被淘汰
    assert(res[0].ok && res[0].delta == 0x400 && res[0].votes == 2);
    assert(res[1].ok && res[1].delta == 0x400 && res[1].votes == 2);
    assert(!res[2].portable && !res[2].ok);
    // 移植後只改了起點，其他不動
    assert(res[0].cheat.opcodes[1] == 0x2400 && res[0].cheat.opcodes[3] == 0x30 && res[0].cheat.opcodes[8] == 999);

    std::printf("跨版本移植測試全部通過\n");
    return 0;
}
