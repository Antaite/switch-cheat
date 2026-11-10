// 進階模式：記憶體數值搜尋（固定記憶體用量）
//
// 四種模式：
//   一般     指定型別搜尋
//   智慧     同時試各種存法：整數、×10、×100、float、double…，結果標出是哪種存法
//   群組     一次輸入 2～3 個畫面上的數字，只找彼此靠在一起的位置（常常一次就找到）
//   未知數值 血條這類沒有數字的東西：先把記憶體存一份到 SD，再用「變大／變小／沒變」縮小範圍
//
// 記憶體預算（固定大小，跟結果多寡無關）：
//   讀取區塊 64KB（＋群組搜尋前後各 512 bytes）、快照比對 64KB、位元表 2×8KB、
//   候選檔讀寫 2×32KB、區段表 16KB，合計約 230KB。候選位址、快照都放 SD 卡。
//
// 不依賴 libnx：透過 MemoryReader 介面讀記憶體，電腦上可以用假記憶體測試。
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include "cheat_core.hpp"

namespace cheattw {

enum class ValueType : uint8_t { U8, U16, U32, U64, F32, F64 };
enum class Cond : uint8_t {
    Equal,       // 等於數值
    Greater,     // 大於數值
    Less,        // 小於數值
    Increased,   // 比上次大（第二次以後才能用）
    Decreased,   // 比上次小
    Changed,     // 有變
    Unchanged,   // 沒變
};
enum class SearchMode : uint8_t { Normal, Smart, Group, Unknown };

const char *valueTypeName(ValueType t);
const char *condName(Cond c);
const char *modeName(SearchMode m);
size_t valueSize(ValueType t);
bool condNeedsValue(Cond c);
bool condNeedsPrevious(Cond c);

// 存法：記憶體裡的值 = 畫面上的值 × scale，用 type 存
struct Encoding {
    ValueType type;
    uint32_t scale;
};
std::string encodingName(const Encoding &e);

struct Region {
    uint64_t base;
    uint64_t size;
};

class MemoryReader {
public:
    virtual ~MemoryReader() = default;
    // 把要搜尋的記憶體區段填進 out，回傳筆數（依位址由小到大）
    virtual size_t regions(Region *out, size_t max) = 0;
    virtual bool read(uint64_t addr, void *buf, size_t size) = 0;
};

// 候選位址：addr 的最高 8 位元存「存法編號」（Switch 位址不會用到那麼高）
struct Candidate {
    uint64_t addr;
    uint64_t value;   // 上次搜尋時的值（原始位元）
};
constexpr uint64_t kAddrMask = 0x00FFFFFFFFFFFFFFull;
inline uint64_t candAddr(const Candidate &c) { return c.addr & kAddrMask; }
inline uint8_t candEnc(const Candidate &c) { return (uint8_t)(c.addr >> 56); }

struct ScanSpec {
    SearchMode mode = SearchMode::Normal;
    ValueType type = ValueType::U32;     // 智慧模式不用
    Cond cond = Cond::Equal;             // 未知數值的第一次不用
    double value = 0;                    // 主數值
    double extra[2] = {0, 0};            // 群組模式的第二、第三個數值
    int extraCount = 0;
    uint32_t window = 256;               // 群組模式：其他數值要在主數值前後多少 bytes 內
};

struct SearchProgress {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
};

enum class SearchResult : uint8_t { Ok, Cancelled, TooMany, IoError, NoRegions, NotStarted, NoSpace, BadSpec };
const char *searchResultText(SearchResult r);

class SearchEngine {
public:
    static constexpr size_t kChunk = 64 * 1024;
    static constexpr size_t kIoBuf = 32 * 1024;
    static constexpr size_t kMaxRegions = 1024;
    static constexpr size_t kMaxWindow = 512;
    static constexpr size_t kMaxEnc = 8;

    // workDir：候選檔、快照放的資料夾（SD 卡）
    void setWorkDir(const std::string &dir) { workDir_ = dir; }
    void setMaxCandidates(uint64_t n) { maxCandidates_ = n; }
    // SD 卡剩餘空間（未知數值模式要先確認放得下快照）；測試時可替換
    void setFreeSpaceFn(uint64_t (*fn)(const std::string &)) { freeSpace_ = fn; }

    SearchResult firstScan(MemoryReader &mem, const ScanSpec &spec, SearchProgress &p);
    SearchResult nextScan(MemoryReader &mem, Cond cond, double value, SearchProgress &p);
    void reset();

    bool started() const { return started_; }
    SearchMode mode() const { return mode_; }
    // 未知數值模式還在「快照」階段（結果太多，還不能列出位址）
    bool inSnapshot() const { return snapshot_; }
    uint64_t count() const { return count_; }
    ValueType type() const { return enc_[0].type; }
    const Encoding &encoding(uint8_t i) const { return enc_[i < encCount_ ? i : 0]; }
    uint32_t snapshotMB() const { return (uint32_t)(snapBytes_ >> 20); }

    // 讀出目前結果的第 offset 筆起、最多 max 筆（給畫面顯示用；快照階段沒有）
    size_t peek(Candidate *out, size_t max, uint64_t offset);
    // 群組模式：找出 addr 附近其他數值的相對位置（顯示用），回傳找到幾個
    int groupNeighbors(MemoryReader &mem, uint64_t addr, int32_t *outOffsets);

    // 存檔／讀檔：overlay 關掉再打開後能接著搜尋。讀檔會確認檔案還在、大小對得上
    bool saveState(const std::string &path) const;
    bool loadState(const std::string &path);

private:
    bool match(const Encoding &e, size_t ei, Cond c, uint64_t cur, uint64_t prev) const;
    void prepareTargets(Cond c, double value);
    std::string pathFor(int slot) const;
    std::string file(const char *name) const { return workDir_ + "/" + name; }
    SearchResult scanCandidatesFirst(MemoryReader &mem, const ScanSpec &spec, SearchProgress &p);
    SearchResult snapshotFirst(MemoryReader &mem, SearchProgress &p);
    SearchResult snapshotNext(MemoryReader &mem, Cond cond, SearchProgress &p);
    void clearSnapshotFiles();

    std::string workDir_ = ".";
    uint64_t maxCandidates_ = 2000000;   // 200 萬筆 × 16 bytes = 32MB 的 SD 檔
    uint64_t (*freeSpace_)(const std::string &) = nullptr;

    SearchMode mode_ = SearchMode::Normal;
    Encoding enc_[kMaxEnc] = {{ValueType::U32, 1}};
    uint8_t encCount_ = 1;
    // 每種存法「等於／大於／小於」要比的原始值（每次搜尋前算好，迴圈裡不用重算）
    uint64_t target_[kMaxEnc] = {};
    bool targetValid_[kMaxEnc] = {};
    double targetValue_ = 0;

    double groupExtra_[2] = {0, 0};
    int groupExtraCount_ = 0;
    uint32_t groupWindow_ = 256;

    uint64_t count_ = 0;
    int slot_ = 0;                       // 目前結果在哪個檔（0/1 輪流用）
    bool started_ = false;
    bool snapshot_ = false;              // 未知數值模式的快照階段
    bool allAlive_ = false;              // 快照後還沒篩過（位元表還沒建立）
    int aliveSlot_ = 0;
    uint64_t snapBytes_ = 0;
    size_t nRegions_ = 0;

    uint8_t chunk_[kChunk + 2 * kMaxWindow];
    uint8_t snap_[kChunk];
    uint8_t bitsIn_[kChunk / 8];
    uint8_t bitsOut_[kChunk / 8];
    uint8_t inBuf_[kIoBuf];
    uint8_t outBuf_[kIoBuf];
    Region regions_[kMaxRegions];
};

// 數值 ↔ 原始位元
uint64_t encodeValue(ValueType t, double v);
double decodeValue(ValueType t, uint64_t bits);
std::string formatValue(ValueType t, uint64_t bits);
// 依存法：畫面值 → 原始位元、原始位元 → 畫面值（文字）
uint64_t encodeDisplay(const Encoding &e, double display);
std::string formatEncoded(const Encoding &e, uint64_t raw);

// 產生「固定寫入數值」的金手指（code type 0）
// regionType：0 = 主程式、1 = heap、2 = alias、3 = aslr
Cheat makeStaticWriteCheat(const std::string &name, uint8_t regionType, uint64_t offset,
                           ValueType t, uint64_t valueBits);

} // namespace cheattw
