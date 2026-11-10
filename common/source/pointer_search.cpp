#include "pointer_search.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace cheattw {

namespace {

struct Node {
    uint64_t loc;       // 這個指標存在哪裡
    uint32_t off;       // 指標值 + off = 上一層的位置
    uint32_t parent;    // 上一層節點的索引（第 1 層的上一層是目標本身）
};

struct Target {
    uint64_t addr;
    uint32_t node;      // 對應到哪個節點（第 0 層 = 目標，固定 0）
    bool operator<(const Target &o) const { return addr < o.addr; }
};

bool read64(MemoryReader &mem, uint64_t addr, uint64_t &out) {
    return mem.read(addr, &out, 8);
}

} // namespace

SearchResult PointerSearch::search(MemoryReader &mem, uint64_t target, const Region &main, const PtrConfig &cfg,
                                   SearchProgress &p, const std::string &outPath, size_t &outCount) {
    outCount = 0;
    std::vector<Region> regions(SearchEngine::kMaxRegions);
    const size_t nRegions = mem.regions(regions.data(), regions.size());
    if (nRegions == 0) return SearchResult::NoRegions;

    FILE *out = std::fopen(outPath.c_str(), "wb");
    if (!out) return SearchResult::IoError;

    uint64_t perPass = 0;
    for (size_t i = 0; i < nRegions; ++i) perPass += regions[i].size;
    p.total = perPass * cfg.maxDepth;
    p.done = 0;

    // levels[d] = 第 d+1 層的節點
    std::vector<std::vector<Node>> levels;
    std::vector<Target> targets = {{target, 0}};
    std::vector<uint8_t> buf(SearchEngine::kChunk);
    SearchResult res = SearchResult::Ok;

    for (int depth = 1; depth <= cfg.maxDepth && res == SearchResult::Ok && !targets.empty(); ++depth) {
        std::sort(targets.begin(), targets.end());
        const uint64_t lo = targets.front().addr >= cfg.maxOffset ? targets.front().addr - cfg.maxOffset : 0;
        const uint64_t hi = targets.back().addr;
        std::vector<Node> nodes;
        nodes.reserve(std::min<size_t>(cfg.maxPerLevel, 1024));

        for (size_t r = 0; r < nRegions && res == SearchResult::Ok; ++r) {
            const Region &rg = regions[r];
            for (uint64_t off = 0; off < rg.size; off += SearchEngine::kChunk) {
                if (p.cancel) { res = SearchResult::Cancelled; break; }
                const size_t n = (size_t)std::min<uint64_t>(SearchEngine::kChunk, rg.size - off);
                p.done += n;
                if (!mem.read(rg.base + off, buf.data(), n)) continue;
                for (size_t i = 0; i + 8 <= n; i += 8) {
                    uint64_t v;
                    std::memcpy(&v, buf.data() + i, 8);
                    if (v < lo || v > hi) continue;   // 大部分的值在這裡就被排除
                    // 找所有 v <= t <= v + maxOffset 的目標
                    auto it = std::lower_bound(targets.begin(), targets.end(), Target{v, 0});
                    for (; it != targets.end() && it->addr - v <= cfg.maxOffset; ++it) {
                        const uint64_t loc = rg.base + off + i;
                        const Node node{loc, (uint32_t)(it->addr - v), it->node};
                        if (loc >= main.base && loc < main.base + main.size) {
                            // 起點在主程式：一條完整路徑。從這層往回串出每層的偏移
                            PtrChain c{};
                            c.depth = (uint8_t)depth;
                            c.baseOff = loc - main.base;
                            c.offs[0] = node.off;
                            uint32_t parent = node.parent;
                            for (int d = depth - 1, k = 1; d >= 1; --d, ++k) {
                                const Node &pn = levels[d - 1][parent];
                                c.offs[k] = pn.off;
                                parent = pn.parent;
                            }
                            if (outCount < cfg.maxChains && std::fwrite(&c, sizeof(c), 1, out) == 1) ++outCount;
                        } else if (nodes.size() < cfg.maxPerLevel) {
                            nodes.push_back(node);
                        }
                    }
                }
            }
        }
        // 下一層要找「指向這一層節點位置」的指標
        targets.clear();
        for (uint32_t i = 0; i < nodes.size(); ++i) targets.push_back({nodes[i].loc, i});
        levels.push_back(std::move(nodes));
    }
    std::fclose(out);
    if (res != SearchResult::Ok) { std::remove(outPath.c_str()); outCount = 0; }
    return res;
}

bool PointerSearch::resolve(MemoryReader &mem, uint64_t mainBase, const PtrChain &c, uint64_t &outAddr) {
    if (c.depth < 1 || c.depth > 4) return false;
    uint64_t ptr;
    if (!read64(mem, mainBase + c.baseOff, ptr)) return false;
    for (int k = 0; k + 1 < c.depth; ++k)
        if (!read64(mem, ptr + c.offs[k], ptr)) return false;
    outAddr = ptr + c.offs[c.depth - 1];
    return true;
}

SearchResult PointerSearch::filter(MemoryReader &mem, uint64_t mainBase, uint64_t newTarget,
                                   const std::string &inPath, const std::string &outPath, size_t &outCount,
                                   SearchProgress &p) {
    outCount = 0;
    FILE *in = std::fopen(inPath.c_str(), "rb");
    if (!in) return SearchResult::NotStarted;
    const std::string tmp = outPath + ".tmp";
    FILE *out = std::fopen(tmp.c_str(), "wb");
    if (!out) { std::fclose(in); return SearchResult::IoError; }
    std::fseek(in, 0, SEEK_END);
    p.total = (uint64_t)std::ftell(in) / sizeof(PtrChain);
    std::fseek(in, 0, SEEK_SET);
    p.done = 0;

    PtrChain c;
    SearchResult res = SearchResult::Ok;
    while (std::fread(&c, sizeof(c), 1, in) == 1) {
        if (p.cancel) { res = SearchResult::Cancelled; break; }
        ++p.done;
        uint64_t a;
        if (resolve(mem, mainBase, c, a) && a == newTarget && std::fwrite(&c, sizeof(c), 1, out) == 1) ++outCount;
    }
    std::fclose(in);
    std::fclose(out);
    if (res != SearchResult::Ok) { std::remove(tmp.c_str()); return res; }
    std::remove(outPath.c_str());
    std::rename(tmp.c_str(), outPath.c_str());
    return SearchResult::Ok;
}

size_t PointerSearch::read(const std::string &path, PtrChain *out, size_t max, size_t offset) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, (long)(offset * sizeof(PtrChain)), SEEK_SET);
    const size_t n = std::fread(out, sizeof(PtrChain), max, f);
    std::fclose(f);
    return n;
}

std::string PointerSearch::describe(const PtrChain &c) {
    char buf[96];
    int len = std::snprintf(buf, sizeof(buf), "M+%llX", (unsigned long long)c.baseOff);
    for (int k = 0; k < c.depth && len < (int)sizeof(buf); ++k)
        len += std::snprintf(buf + len, sizeof(buf) - len, " → +%X", c.offs[k]);
    return buf;
}

Cheat PointerSearch::makeCheat(const std::string &name, const PtrChain &c, ValueType t, uint64_t valueBits) {
    Cheat ch;
    ch.name = name;
    // 580F0000 AAAAAAAA：F = [主程式 + A]
    ch.opcodes.push_back(0x580F0000u | (uint32_t)((c.baseOff >> 32) & 0xFF));
    ch.opcodes.push_back((uint32_t)c.baseOff);
    // 580F1000 AAAAAAAA：F = [F + A]
    for (int k = 0; k + 1 < c.depth; ++k) {
        ch.opcodes.push_back(0x580F1000u);
        ch.opcodes.push_back(c.offs[k]);
    }
    // 780F0000 VVVVVVVV：F += V
    ch.opcodes.push_back(0x780F0000u);
    ch.opcodes.push_back(c.offs[c.depth - 1]);
    // 6T0F0000 VVVVVVVV VVVVVVVV：[F] = V（T = 寬度）
    ch.opcodes.push_back(0x600F0000u | ((uint32_t)valueSize(t) << 24));
    ch.opcodes.push_back((uint32_t)(valueBits >> 32));
    ch.opcodes.push_back((uint32_t)valueBits);
    return ch;
}

} // namespace cheattw
