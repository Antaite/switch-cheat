#include "search_engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace cheattw {

const char *valueTypeName(ValueType t) {
    switch (t) {
        case ValueType::U8:  return "整數 1 byte";
        case ValueType::U16: return "整數 2 bytes";
        case ValueType::U32: return "整數 4 bytes";
        case ValueType::U64: return "整數 8 bytes";
        case ValueType::F32: return "小數 float";
        case ValueType::F64: return "小數 double";
    }
    return "?";
}

const char *condName(Cond c) {
    switch (c) {
        case Cond::Equal:     return "等於";
        case Cond::Greater:   return "大於";
        case Cond::Less:      return "小於";
        case Cond::Increased: return "變大了";
        case Cond::Decreased: return "變小了";
        case Cond::Changed:   return "有變";
        case Cond::Unchanged: return "沒變";
    }
    return "?";
}

size_t valueSize(ValueType t) {
    switch (t) {
        case ValueType::U8: return 1;
        case ValueType::U16: return 2;
        case ValueType::U32: case ValueType::F32: return 4;
        default: return 8;
    }
}

bool condNeedsValue(Cond c) { return c == Cond::Equal || c == Cond::Greater || c == Cond::Less; }
bool condNeedsPrevious(Cond c) { return !condNeedsValue(c); }

const char *searchResultText(SearchResult r) {
    switch (r) {
        case SearchResult::Ok:         return "完成";
        case SearchResult::Cancelled:  return "已取消";
        case SearchResult::TooMany:    return "結果超過 200 萬筆，這個數值太常見。請換一個比較少見的數值（例如金錢 12345 比 100 好找）";
        case SearchResult::IoError:    return "SD 卡讀寫失敗";
        case SearchResult::NoRegions:  return "讀不到遊戲記憶體";
        case SearchResult::NotStarted: return "請先做第一次搜尋";
    }
    return "?";
}

namespace {

uint64_t load(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    std::memcpy(&v, p, n);   // Switch 是 little-endian，直接複製即可
    return v;
}

double asDouble(ValueType t, uint64_t bits) {
    if (t == ValueType::F32) { float f; uint32_t b = (uint32_t)bits; std::memcpy(&f, &b, 4); return f; }
    if (t == ValueType::F64) { double d; std::memcpy(&d, &bits, 8); return d; }
    return (double)bits;
}

bool isFloat(ValueType t) { return t == ValueType::F32 || t == ValueType::F64; }

// 緩衝寫檔：固定 32KB，滿了才寫一次 SD
struct BufWriter {
    FILE *f; uint8_t *buf; size_t cap, len = 0; bool ok = true;
    BufWriter(FILE *f_, uint8_t *b, size_t c) : f(f_), buf(b), cap(c) {}
    void put(const Candidate &c) {
        if (len + sizeof(c) > cap) flush();
        std::memcpy(buf + len, &c, sizeof(c));
        len += sizeof(c);
    }
    void flush() {
        if (len && std::fwrite(buf, 1, len, f) != len) ok = false;
        len = 0;
    }
};

struct BufReader {
    FILE *f; uint8_t *buf; size_t cap, len = 0, pos = 0;
    BufReader(FILE *f_, uint8_t *b, size_t c) : f(f_), buf(b), cap(c) {}
    bool next(Candidate &c) {
        if (pos + sizeof(c) > len) {
            len = std::fread(buf, 1, cap - cap % sizeof(c), f);
            pos = 0;
            if (len < sizeof(c)) return false;
        }
        std::memcpy(&c, buf + pos, sizeof(c));
        pos += sizeof(c);
        return true;
    }
};

} // namespace

bool SearchEngine::match(Cond c, uint64_t cur, uint64_t prev, uint64_t target) const {
    if (isFloat(type_)) {
        double a = asDouble(type_, cur), b = asDouble(type_, target), p = asDouble(type_, prev);
        if (std::isnan(a)) return false;
        switch (c) {
            case Cond::Equal:     return std::fabs(a - b) <= std::fmax(1e-3, std::fabs(b) * 1e-4);
            case Cond::Greater:   return a > b;
            case Cond::Less:      return a < b;
            case Cond::Increased: return a > p;
            case Cond::Decreased: return a < p;
            case Cond::Changed:   return cur != prev;
            case Cond::Unchanged: return cur == prev;
        }
        return false;
    }
    switch (c) {
        case Cond::Equal:     return cur == target;
        case Cond::Greater:   return cur > target;
        case Cond::Less:      return cur < target;
        case Cond::Increased: return cur > prev;
        case Cond::Decreased: return cur < prev;
        case Cond::Changed:   return cur != prev;
        case Cond::Unchanged: return cur == prev;
    }
    return false;
}

std::string SearchEngine::pathFor(int slot) const {
    return workDir_ + (slot ? "/cand1.bin" : "/cand0.bin");
}

void SearchEngine::reset() {
    std::remove(pathFor(0).c_str());
    std::remove(pathFor(1).c_str());
    started_ = false;
    count_ = 0;
}

SearchResult SearchEngine::firstScan(MemoryReader &mem, ValueType type, Cond cond, uint64_t valueBits,
                                     SearchProgress &p) {
    if (condNeedsPrevious(cond)) return SearchResult::NotStarted;   // 第一次沒有「上次的值」可以比
    const size_t nRegions = mem.regions(regions_, kMaxRegions);
    if (nRegions == 0) return SearchResult::NoRegions;

    const ValueType oldType = type_;
    type_ = type;
    const size_t w = valueSize(type);
    uint64_t total = 0;
    for (size_t i = 0; i < nRegions; ++i) total += regions_[i].size;
    p.total = total;
    p.done = 0;

    const int outSlot = started_ ? 1 - slot_ : 0;
    FILE *f = std::fopen(pathFor(outSlot).c_str(), "wb");
    if (!f) { type_ = oldType; return SearchResult::IoError; }
    BufWriter out(f, outBuf_, kIoBuf);
    uint64_t found = 0;
    SearchResult res = SearchResult::Ok;

    for (size_t r = 0; r < nRegions && res == SearchResult::Ok; ++r) {
        const Region &rg = regions_[r];
        for (uint64_t off = 0; off < rg.size; off += kChunk) {
            if (p.cancel) { res = SearchResult::Cancelled; break; }
            const size_t n = (size_t)std::min<uint64_t>(kChunk, rg.size - off);
            p.done += n;
            if (!mem.read(rg.base + off, chunk_, n)) continue;          // 讀不到的頁面直接跳過
            for (size_t i = 0; i + w <= n; i += w) {                    // 依數值大小對齊
                const uint64_t v = load(chunk_ + i, w);
                if (!match(cond, v, 0, valueBits)) continue;
                if (++found > maxCandidates_) { res = SearchResult::TooMany; break; }
                out.put({rg.base + off + i, v});
            }
            if (res != SearchResult::Ok) break;
        }
    }
    out.flush();
    std::fclose(f);

    if (res != SearchResult::Ok || !out.ok) {
        std::remove(pathFor(outSlot).c_str());
        type_ = oldType;                     // 失敗就保留上一次的結果
        return out.ok ? res : SearchResult::IoError;
    }
    if (started_) std::remove(pathFor(slot_).c_str());
    slot_ = outSlot;
    count_ = found;
    started_ = true;
    return SearchResult::Ok;
}

SearchResult SearchEngine::nextScan(MemoryReader &mem, Cond cond, uint64_t valueBits, SearchProgress &p) {
    if (!started_) return SearchResult::NotStarted;
    const size_t nRegions = mem.regions(regions_, kMaxRegions);
    const size_t w = valueSize(type_);
    p.total = count_;
    p.done = 0;

    FILE *in = std::fopen(pathFor(slot_).c_str(), "rb");
    if (!in) return SearchResult::IoError;
    const int outSlot = 1 - slot_;
    FILE *outF = std::fopen(pathFor(outSlot).c_str(), "wb");
    if (!outF) { std::fclose(in); return SearchResult::IoError; }

    BufReader rd(in, inBuf_, kIoBuf);
    BufWriter out(outF, outBuf_, kIoBuf);

    // 候選依位址排序，所以可以一次讀 64KB 的視窗，同一段內的候選都從這個視窗取值
    uint64_t winBase = 0, winLen = 0;
    bool winOk = false;
    size_t rIdx = 0;
    uint64_t kept = 0;
    Candidate c;
    SearchResult res = SearchResult::Ok;

    while (rd.next(c)) {
        if ((++p.done & 0x3FF) == 0 && p.cancel) { res = SearchResult::Cancelled; break; }
        if (!(winLen && c.addr >= winBase && c.addr + w <= winBase + winLen)) {
            // 找出這個位址所在的區段，視窗不能跨出區段（跨出去會讀失敗）
            while (rIdx < nRegions && regions_[rIdx].base + regions_[rIdx].size <= c.addr) ++rIdx;
            if (rIdx >= nRegions || c.addr < regions_[rIdx].base) { winLen = 0; continue; }   // 這段記憶體已經不在了
            const uint64_t regEnd = regions_[rIdx].base + regions_[rIdx].size;
            winBase = c.addr;
            winLen = std::min<uint64_t>(kChunk, regEnd - c.addr);
            winOk = mem.read(winBase, chunk_, (size_t)winLen);
        }
        if (!winOk || c.addr + w > winBase + winLen) continue;
        const uint64_t v = load(chunk_ + (c.addr - winBase), w);
        if (!match(cond, v, c.value, valueBits)) continue;
        out.put({c.addr, v});
        ++kept;
    }
    out.flush();
    std::fclose(in);
    std::fclose(outF);

    if (res != SearchResult::Ok || !out.ok) {
        std::remove(pathFor(outSlot).c_str());
        return out.ok ? res : SearchResult::IoError;
    }
    std::remove(pathFor(slot_).c_str());
    slot_ = outSlot;
    count_ = kept;
    return SearchResult::Ok;
}

namespace {
struct SavedEngine {
    uint32_t magic;      // 'CTWS'
    uint32_t version;
    uint8_t type;
    uint8_t slot;
    uint8_t pad[6];
    uint64_t count;
};
constexpr uint32_t kMagic = 0x53575443, kVersion = 1;
}

bool SearchEngine::saveState(const std::string &path) const {
    if (!started_) { std::remove(path.c_str()); return true; }
    SavedEngine st{kMagic, kVersion, (uint8_t)type_, (uint8_t)slot_, {}, count_};
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(&st, sizeof(st), 1, f) == 1;
    std::fclose(f);
    return ok;
}

bool SearchEngine::loadState(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    SavedEngine st{};
    const bool ok = std::fread(&st, sizeof(st), 1, f) == 1;
    std::fclose(f);
    if (!ok || st.magic != kMagic || st.version != kVersion || st.slot > 1 || st.type > (uint8_t)ValueType::F64) return false;

    // 候選檔要還在，而且大小剛好是筆數 × 16 bytes
    FILE *c = std::fopen(pathFor(st.slot).c_str(), "rb");
    if (!c) return false;
    std::fseek(c, 0, SEEK_END);
    const long size = std::ftell(c);
    std::fclose(c);
    if (size < 0 || (uint64_t)size != st.count * sizeof(Candidate)) return false;

    type_ = (ValueType)st.type;
    slot_ = st.slot;
    count_ = st.count;
    started_ = true;
    return true;
}

size_t SearchEngine::peek(Candidate *out, size_t max, uint64_t offset) {
    if (!started_ || offset >= count_) return 0;
    FILE *f = std::fopen(pathFor(slot_).c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, (long)(offset * sizeof(Candidate)), SEEK_SET);
    size_t n = std::fread(out, sizeof(Candidate), max, f);
    std::fclose(f);
    return n;
}

uint64_t encodeValue(ValueType t, double v) {
    switch (t) {
        case ValueType::F32: { float f = (float)v; uint32_t b; std::memcpy(&b, &f, 4); return b; }
        case ValueType::F64: { uint64_t b; std::memcpy(&b, &v, 8); return b; }
        case ValueType::U8:  return (uint64_t)(int64_t)v & 0xFF;
        case ValueType::U16: return (uint64_t)(int64_t)v & 0xFFFF;
        case ValueType::U32: return (uint64_t)(int64_t)v & 0xFFFFFFFFull;
        case ValueType::U64: return (uint64_t)(int64_t)v;
    }
    return 0;
}

double decodeValue(ValueType t, uint64_t bits) { return asDouble(t, bits); }

std::string formatValue(ValueType t, uint64_t bits) {
    char buf[40];
    if (isFloat(t)) std::snprintf(buf, sizeof(buf), "%.4g", asDouble(t, bits));
    else std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)bits);
    return buf;
}

Cheat makeStaticWriteCheat(const std::string &name, uint8_t regionType, uint64_t offset,
                           ValueType t, uint64_t valueBits) {
    // 0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV)
    // T = 寬度、M = 記憶體區（0 主程式／1 heap）、R = 偏移暫存器（不用）、A = 40-bit 偏移
    Cheat c;
    c.name = name;
    const uint32_t width = (uint32_t)valueSize(t);
    c.opcodes.push_back((width << 24) | ((uint32_t)(regionType & 0xF) << 20) | (uint32_t)((offset >> 32) & 0xFF));
    c.opcodes.push_back((uint32_t)(offset & 0xFFFFFFFFu));
    if (width == 8) c.opcodes.push_back((uint32_t)(valueBits >> 32));
    c.opcodes.push_back((uint32_t)(valueBits & 0xFFFFFFFFu));
    return c;
}

} // namespace cheattw
