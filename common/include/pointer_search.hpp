// 指標搜尋：找出「從主程式固定位置出發，經過幾層指標到達目標位址」的路徑。
// 這樣產生的金手指重開遊戲也能用（一般鎖定的位址每次開遊戲都會變）。
//
// 做法（反向逐層）：
//   第 1 層：掃整個記憶體，找「值落在 [目標 − 最大偏移, 目標]」的 8-byte 指標
//   第 2 層：再找指向第 1 層那些位置的指標……最多 3 層
//   指標本身放在主程式範圍內 → 找到一條完整路徑
//
// 記憶體：每層最多記 4096 個節點（每個 16 bytes），只在搜尋時配置，搜完就釋放；
//         讀取用 64KB 區塊；找到的路徑直接寫進 SD 卡。
#pragma once
#include <cstdint>
#include <string>
#include "search_engine.hpp"

namespace cheattw {

struct PtrChain {
    uint8_t depth;          // 幾層指標（1～4）
    uint8_t pad[3];
    uint32_t offs[4];       // offs[0..depth-2]：每層解參照前加的偏移；offs[depth-1]：最後加到目標的偏移
    uint64_t baseOff;       // 起點 = 主程式基址 + baseOff
};

struct PtrConfig {
    int maxDepth = 3;
    uint32_t maxOffset = 0x800;
    size_t maxPerLevel = 4096;
    size_t maxChains = 20000;
};

class PointerSearch {
public:
    // regions：要掃的記憶體（遊戲資料區 + 主程式）；main：主程式範圍
    static SearchResult search(MemoryReader &mem, uint64_t target, const Region &main, const PtrConfig &cfg,
                               SearchProgress &p, const std::string &outPath, size_t &outCount);
    // 重開遊戲後：只留下解出來剛好等於新位址的路徑
    static SearchResult filter(MemoryReader &mem, uint64_t mainBase, uint64_t newTarget,
                               const std::string &inPath, const std::string &outPath, size_t &outCount,
                               SearchProgress &p);
    static bool resolve(MemoryReader &mem, uint64_t mainBase, const PtrChain &c, uint64_t &outAddr);
    static size_t read(const std::string &path, PtrChain *out, size_t max, size_t offset);
    // 例：M+0x1234 → +0x40 → +0x10
    static std::string describe(const PtrChain &c);
    // 產生金手指：F = [main+base]；F = [F+off]…；F += 最後偏移；[F] = 數值
    static Cheat makeCheat(const std::string &name, const PtrChain &c, ValueType t, uint64_t valueBits);
};

} // namespace cheattw
