#include "porter.hpp"
#include <algorithm>
#include <cstring>
#include <map>

namespace cheattw {

namespace {

bool isValid(const std::vector<Region> &valid, uint64_t v) {
    auto it = std::upper_bound(valid.begin(), valid.end(), v,
                               [](uint64_t x, const Region &r) { return x < r.base; });
    if (it == valid.begin()) return false;
    --it;
    return v >= it->base && v < it->base + it->size;
}

// 從主程式位址 x 出發，照偏移一路讀下去，每層都要是有效指標
bool follow(MemoryReader &mem, const std::vector<Region> &valid, uint64_t x, const std::vector<uint64_t> &derefs) {
    uint64_t v;
    if (!mem.read(x, &v, 8) || !isValid(valid, v)) return false;
    for (uint64_t o : derefs) {
        if (!mem.read(v + o, &v, 8) || !isValid(valid, v)) return false;
    }
    return true;
}

// 在 [lo, hi) 內所有主程式可讀寫的位置找候選起點（回傳相對主程式的偏移）
void scanCandidates(MemoryReader &mem, uint64_t mainBase, const std::vector<Region> &mainRw,
                    const std::vector<Region> &valid, const std::vector<uint64_t> &derefs,
                    uint64_t lo, uint64_t hi, std::vector<uint64_t> &out, size_t max, SearchProgress &p) {
    static uint8_t buf[SearchEngine::kChunk];
    for (const Region &r : mainRw) {
        const uint64_t s = std::max(lo, r.base), e = std::min(hi, r.base + r.size);
        for (uint64_t a = s & ~7ull; a < e && out.size() < max; a += SearchEngine::kChunk) {
            if (p.cancel) return;
            const size_t n = (size_t)std::min<uint64_t>(SearchEngine::kChunk, e - a);
            if (!mem.read(a, buf, n)) continue;
            for (size_t i = 0; i + 8 <= n && out.size() < max; i += 8) {
                uint64_t v;
                std::memcpy(&v, buf + i, 8);
                if (!isValid(valid, v)) continue;              // 先用值本身篩掉大部分
                if (follow(mem, valid, a + i, derefs)) out.push_back(a + i - mainBase);
            }
        }
    }
}

} // namespace

bool Porter::parse(const Cheat &c, uint64_t &base, uint8_t &reg, std::vector<uint64_t> &derefs) {
    derefs.clear();
    const auto &op = c.opcodes;
    if (op.size() < 2) return false;
    // 5TMR00AA AAAAAAAA，M = 0（主程式），第 5 個字元 = 0（從記憶體載入）
    const uint32_t w = op[0];
    if ((w >> 28) != 5 || ((w >> 20) & 0xF) != 0 || ((w >> 12) & 0xF) != 0 || ((w >> 24) & 0xF) != 8) return false;
    reg = (uint8_t)((w >> 16) & 0xF);
    base = ((uint64_t)(w & 0xFF) << 32) | op[1];
    // 5T0R10AA AAAAAAAA：R = [R + A]（同一個暫存器）
    for (size_t i = 2; i + 1 < op.size(); i += 2) {
        const uint32_t x = op[i];
        if ((x >> 28) != 5 || ((x >> 16) & 0xF) != reg || ((x >> 12) & 0xF) != 1 || ((x >> 20) & 0xF) != 0) break;
        derefs.push_back(((uint64_t)(x & 0xFF) << 32) | op[i + 1]);
    }
    return true;
}

std::vector<PortResult> Porter::portAll(MemoryReader &mem, uint64_t mainBase,
                                        const std::vector<Region> &mainRw, const std::vector<Region> &valid,
                                        const std::vector<Cheat> &cheats, uint64_t window, SearchProgress &p) {
    struct Item { size_t idx; uint64_t base; std::vector<uint64_t> derefs; std::vector<int64_t> deltas; };
    std::vector<PortResult> results(cheats.size());
    std::vector<Item> items;
    for (size_t i = 0; i < cheats.size(); ++i) {
        results[i].name = cheats[i].name;
        uint64_t base;
        uint8_t reg;
        std::vector<uint64_t> d;
        if (parse(cheats[i], base, reg, d)) {
            results[i].portable = true;
            items.push_back({i, base, d, {}});
        }
    }
    p.total = items.size();
    p.done = 0;

    std::map<int64_t, int> votes;
    // 同一個起點＋偏移的碼只算一次
    std::map<std::pair<uint64_t, std::vector<uint64_t>>, std::vector<int64_t>> cache;

    for (auto &it : items) {
        if (p.cancel) break;
        auto key = std::make_pair(it.base, it.derefs);
        auto hit = cache.find(key);
        if (hit != cache.end()) {
            it.deltas = hit->second;
        } else {
            // 先試目前票數最高的幾個位移量：對得上就不用整段掃描
            std::vector<std::pair<int, int64_t>> ranked;
            for (auto &kv : votes) ranked.push_back({kv.second, kv.first});
            std::sort(ranked.rbegin(), ranked.rend());
            for (size_t k = 0; k < ranked.size() && k < 8; ++k) {
                const int64_t d = ranked[k].second;
                const uint64_t x = mainBase + it.base + d;
                if (follow(mem, valid, x, it.derefs)) it.deltas.push_back(d);
            }
            if (it.deltas.empty()) {
                std::vector<uint64_t> cand;
                const uint64_t old = mainBase + it.base;
                scanCandidates(mem, mainBase, mainRw, valid, it.derefs,
                               old > window ? old - window : 0, old + window, cand, 64, p);
                for (uint64_t c : cand) it.deltas.push_back((int64_t)c - (int64_t)it.base);
            }
            cache[key] = it.deltas;
        }
        for (int64_t d : it.deltas) ++votes[d];
        ++p.done;
    }

    // 選位移量：票數最多的；同票就選離舊位置最近的
    auto pick = [&](const std::vector<int64_t> &ds, int &v) {
        int64_t best = 0;
        v = -1;
        for (int64_t d : ds) {
            const int n = votes[d];
            if (n > v || (n == v && std::llabs(d) < std::llabs(best))) { best = d; v = n; }
        }
        return best;
    };
    for (auto &it : items) {
        PortResult &r = results[it.idx];
        if (it.deltas.empty()) continue;
        int v;
        const int64_t d = pick(it.deltas, v);
        r.ok = true;
        r.delta = d;
        r.votes = v;
        r.cheat = cheats[it.idx];
        const uint64_t nb = (uint64_t)((int64_t)it.base + d);
        r.cheat.opcodes[0] = (r.cheat.opcodes[0] & ~0xFFu) | (uint32_t)((nb >> 32) & 0xFF);
        r.cheat.opcodes[1] = (uint32_t)nb;
    }
    return results;
}

} // namespace cheattw
