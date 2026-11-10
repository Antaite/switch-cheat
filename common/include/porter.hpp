// 跨版本移植：把其他版本的「指標碼」改成能在你的版本用。
//
// 指標碼長這樣：580F0000 起點偏移 → 580F1000 偏移 → … → 寫入
// 遊戲更新後，通常只有「起點偏移」（主程式裡存著指標的那個變數）換了位置，後面每層偏移不變。
//
// 做法：
//   1. 在主程式的可讀寫區段裡、舊起點前後一段範圍內，找出每個 8-byte 值
//   2. 照舊碼的每層偏移一路讀下去，每一層都要落在有效的記憶體裡 → 這個位置就是候選起點
//   3. 同一個舊版本的多條碼，起點位移量通常一樣：用「投票」選出最多碼支持的位移量
//      → 投票數 ≥ 2：信心高；只有自己一條：信心低（要自己試）
//   同一個位移量能直接驗證的碼就不再整段掃描，所以碼越多反而越快。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "search_engine.hpp"

namespace cheattw {

struct PortResult {
    std::string name;
    bool portable = false;     // 這條碼是不是指標碼（不是的話不能移植）
    bool ok = false;           // 找到新起點
    int votes = 0;             // 支持這個位移量的碼有幾條
    int64_t delta = 0;         // 新起點 − 舊起點
    Cheat cheat;               // 移植後的碼
};

class Porter {
public:
    // 解析指標碼：第一條必須是「從主程式載入」，接著可以有幾層「從暫存器載入」
    static bool parse(const Cheat &c, uint64_t &base, uint8_t &reg, std::vector<uint64_t> &derefs);

    // mainBase：主程式基址；mainRw：主程式可讀寫區段；valid：指標可以指去的範圍（依位址排序）
    // window：在舊起點前後多少 bytes 內找
    static std::vector<PortResult> portAll(MemoryReader &mem, uint64_t mainBase,
                                           const std::vector<Region> &mainRw, const std::vector<Region> &valid,
                                           const std::vector<Cheat> &cheats, uint64_t window, SearchProgress &p);
};

} // namespace cheattw
