// 進階模式：記憶體數值搜尋（固定記憶體用量）
//
// 記憶體預算（全部是固定大小，搜尋多少筆都不會變多）：
//   讀取區塊      64 KB   每次只從遊戲讀 64KB
//   候選檔讀寫    2 × 32 KB 緩衝，候選位址存在 SD 卡，不放記憶體
//   區段表        最多 1024 段 × 16 bytes = 16 KB
//   合計約 150 KB，Tesla overlay 的 heap 只有幾 MB，這樣很安全
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

const char *valueTypeName(ValueType t);
const char *condName(Cond c);
size_t valueSize(ValueType t);
bool condNeedsValue(Cond c);
bool condNeedsPrevious(Cond c);

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

struct Candidate {
    uint64_t addr;
    uint64_t value;   // 上次搜尋時的值（原始位元）
};

struct SearchProgress {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
};

enum class SearchResult : uint8_t { Ok, Cancelled, TooMany, IoError, NoRegions, NotStarted };
const char *searchResultText(SearchResult r);

class SearchEngine {
public:
    static constexpr size_t kChunk = 64 * 1024;
    static constexpr size_t kIoBuf = 32 * 1024;
    static constexpr size_t kMaxRegions = 1024;

    // workDir：候選檔放的資料夾（SD 卡）
    void setWorkDir(const std::string &dir) { workDir_ = dir; }
    void setMaxCandidates(uint64_t n) { maxCandidates_ = n; }

    SearchResult firstScan(MemoryReader &mem, ValueType type, Cond cond, uint64_t valueBits, SearchProgress &p);
    SearchResult nextScan(MemoryReader &mem, Cond cond, uint64_t valueBits, SearchProgress &p);
    void reset();

    bool started() const { return started_; }
    uint64_t count() const { return count_; }
    ValueType type() const { return type_; }
    // 讀出目前結果的第 offset 筆起、最多 max 筆（給畫面顯示用）
    size_t peek(Candidate *out, size_t max, uint64_t offset);

private:
    bool match(Cond c, uint64_t cur, uint64_t prev, uint64_t target) const;
    std::string pathFor(int slot) const;

    std::string workDir_ = ".";
    uint64_t maxCandidates_ = 2000000;   // 200 萬筆 × 16 bytes = 32MB 的 SD 檔，超過就請使用者換數值
    ValueType type_ = ValueType::U32;
    uint64_t count_ = 0;
    int slot_ = 0;                       // 目前結果在哪個檔（0/1 輪流用）
    bool started_ = false;

    uint8_t chunk_[kChunk];
    uint8_t inBuf_[kIoBuf];
    uint8_t outBuf_[kIoBuf];
    Region regions_[kMaxRegions];
};

// 數值 ↔ 原始位元
uint64_t encodeValue(ValueType t, double v);
double decodeValue(ValueType t, uint64_t bits);
std::string formatValue(ValueType t, uint64_t bits);

// 產生「固定寫入數值」的金手指（code type 0）
// regionType：0 = 主程式（main NSO），1 = heap
Cheat makeStaticWriteCheat(const std::string &name, uint8_t regionType, uint64_t offset,
                           ValueType t, uint64_t valueBits);

} // namespace cheattw
