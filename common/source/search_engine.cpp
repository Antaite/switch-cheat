#include "search_engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sys/statvfs.h>

namespace cheattw {

// ───────────────────────── 名稱 ─────────────────────────

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

const char *modeName(SearchMode m) {
    switch (m) {
        case SearchMode::Normal:  return "一般";
        case SearchMode::Smart:   return "智慧（自動試各種存法）";
        case SearchMode::Group:   return "群組（多個數值）";
        case SearchMode::Unknown: return "未知數值（血條等）";
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

std::string encodingName(const Encoding &e) {
    std::string s;
    switch (e.type) {
        case ValueType::U8:  s = "整數1"; break;
        case ValueType::U16: s = "整數2"; break;
        case ValueType::U32: s = "整數4"; break;
        case ValueType::U64: s = "整數8"; break;
        case ValueType::F32: s = "float"; break;
        case ValueType::F64: s = "double"; break;
    }
    if (e.scale != 1) s += "×" + std::to_string(e.scale);
    return s;
}

const char *searchResultText(SearchResult r) {
    switch (r) {
        case SearchResult::Ok:         return "完成";
        case SearchResult::Cancelled:  return "已取消";
        case SearchResult::TooMany:    return "結果超過 200 萬筆，這個數值太常見。請換一個比較少見的數值，或改用群組搜尋";
        case SearchResult::IoError:    return "SD 卡讀寫失敗";
        case SearchResult::NoRegions:  return "讀不到遊戲記憶體";
        case SearchResult::NotStarted: return "請先做第一次搜尋";
        case SearchResult::NoSpace:    return "SD 卡空間不夠存記憶體快照";
        case SearchResult::BadSpec:    return "這個模式不能用這種比較方式";
    }
    return "?";
}

// ───────────────────────── 小工具 ─────────────────────────

namespace {

uint64_t load(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    std::memcpy(&v, p, n);   // Switch 是 little-endian，直接複製即可
    return v;
}

bool isFloat(ValueType t) { return t == ValueType::F32 || t == ValueType::F64; }

double asDouble(ValueType t, uint64_t bits) {
    if (t == ValueType::F32) { float f; uint32_t b = (uint32_t)bits; std::memcpy(&f, &b, 4); return f; }
    if (t == ValueType::F64) { double d; std::memcpy(&d, &bits, 8); return d; }
    return (double)bits;
}

uint64_t widthMask(ValueType t) {
    const size_t w = valueSize(t);
    return w == 8 ? ~0ull : ((1ull << (w * 8)) - 1);
}

uint64_t defaultFreeSpace(const std::string &path) {
    struct statvfs st;
    if (statvfs(path.c_str(), &st) != 0) return ~0ull;   // 查不到就不擋
    return (uint64_t)st.f_bavail * st.f_bsize;
}

// 緩衝寫檔：固定大小，滿了才寫一次 SD
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

uint64_t encodeValue(ValueType t, double v) {
    switch (t) {
        case ValueType::F32: { float f = (float)v; uint32_t b; std::memcpy(&b, &f, 4); return b; }
        case ValueType::F64: { uint64_t b; std::memcpy(&b, &v, 8); return b; }
        default: return (uint64_t)(int64_t)std::llround(v) & widthMask(t);
    }
}

double decodeValue(ValueType t, uint64_t bits) { return asDouble(t, bits); }

std::string formatValue(ValueType t, uint64_t bits) {
    char buf[40];
    if (isFloat(t)) std::snprintf(buf, sizeof(buf), "%.4g", asDouble(t, bits));
    else std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)bits);
    return buf;
}

uint64_t encodeDisplay(const Encoding &e, double display) {
    return encodeValue(e.type, display * e.scale);
}

std::string formatEncoded(const Encoding &e, uint64_t raw) {
    if (e.scale == 1 || isFloat(e.type)) return formatValue(e.type, raw);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.4g", (double)raw / e.scale);
    return buf;
}

// ───────────────────────── 比對 ─────────────────────────

void SearchEngine::prepareTargets(Cond c, double value) {
    targetValue_ = value;
    for (size_t i = 0; i < encCount_; ++i) {
        const Encoding &e = enc_[i];
        const double scaled = value * e.scale;
        targetValid_[i] = true;
        if (!isFloat(e.type)) {
            // 整數存法：等於時乘完倍率一定要是整數，而且放得進這個寬度
            if (c == Cond::Equal && std::fabs(scaled - std::round(scaled)) > 1e-6) targetValid_[i] = false;
            if (scaled < 0 && valueSize(e.type) < 8 && -scaled > (double)(widthMask(e.type) >> 1)) targetValid_[i] = false;
            if (scaled > (double)widthMask(e.type)) targetValid_[i] = false;
        }
        target_[i] = encodeValue(e.type, scaled);
    }
}

bool SearchEngine::match(const Encoding &e, size_t ei, Cond c, uint64_t cur, uint64_t prev) const {
    if (isFloat(e.type)) {
        const double a = asDouble(e.type, cur), p = asDouble(e.type, prev), b = targetValue_;
        if (std::isnan(a) || std::isinf(a)) return false;
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
        case Cond::Equal:     return targetValid_[ei] && cur == target_[ei];
        case Cond::Greater:   return cur > target_[ei];
        case Cond::Less:      return cur < target_[ei];
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

void SearchEngine::clearSnapshotFiles() {
    std::remove(file("snap.bin").c_str());
    std::remove(file("alive0.bin").c_str());
    std::remove(file("alive1.bin").c_str());
    std::remove(file("regions.bin").c_str());
}

void SearchEngine::reset() {
    std::remove(pathFor(0).c_str());
    std::remove(pathFor(1).c_str());
    clearSnapshotFiles();
    started_ = snapshot_ = allAlive_ = false;
    count_ = 0;
    snapBytes_ = 0;
}

// ───────────────────────── 第一次搜尋 ─────────────────────────

SearchResult SearchEngine::firstScan(MemoryReader &mem, const ScanSpec &spec, SearchProgress &p) {
    if (spec.mode != SearchMode::Unknown && condNeedsPrevious(spec.cond)) return SearchResult::NotStarted;
    if (spec.mode == SearchMode::Group && (spec.cond != Cond::Equal || spec.extraCount < 1)) return SearchResult::BadSpec;
    if (spec.mode == SearchMode::Smart && spec.cond != Cond::Equal) return SearchResult::BadSpec;

    nRegions_ = mem.regions(regions_, kMaxRegions);
    if (nRegions_ == 0) return SearchResult::NoRegions;

    // 舊結果先留著，新搜尋成功才換掉
    const SearchMode oldMode = mode_;
    Encoding oldEnc[kMaxEnc];
    std::memcpy(oldEnc, enc_, sizeof(enc_));
    const uint8_t oldEncCount = encCount_;

    mode_ = spec.mode;
    encCount_ = 0;
    if (spec.mode == SearchMode::Smart) {
        // 同時試的存法，依常見程度排序（同一個位址只記第一個對上的）
        const double v = spec.value;
        enc_[encCount_++] = {ValueType::U32, 1};
        enc_[encCount_++] = {ValueType::F32, 1};
        enc_[encCount_++] = {ValueType::U32, 10};
        enc_[encCount_++] = {ValueType::U32, 100};
        enc_[encCount_++] = {ValueType::F64, 1};
        if (v >= 256 && v <= 65535) enc_[encCount_++] = {ValueType::U16, 1};   // 太小的數值用 2 bytes 會多出一大堆
        if (v > 4294967295.0) enc_[encCount_++] = {ValueType::U64, 1};
    } else {
        enc_[encCount_++] = {spec.type, 1};
    }

    SearchResult r;
    if (spec.mode == SearchMode::Unknown) {
        r = snapshotFirst(mem, p);
    } else {
        groupExtraCount_ = spec.mode == SearchMode::Group ? std::min(spec.extraCount, 2) : 0;
        for (int i = 0; i < groupExtraCount_; ++i) groupExtra_[i] = spec.extra[i];
        groupWindow_ = std::min<uint32_t>(spec.window, kMaxWindow);
        prepareTargets(spec.cond, spec.value);
        r = scanCandidatesFirst(mem, spec, p);
    }
    if (r != SearchResult::Ok) {
        mode_ = oldMode;
        std::memcpy(enc_, oldEnc, sizeof(enc_));
        encCount_ = oldEncCount;
    }
    return r;
}

SearchResult SearchEngine::scanCandidatesFirst(MemoryReader &mem, const ScanSpec &spec, SearchProgress &p) {
    // 位址每次前進多少：所有存法裡最小的寬度
    size_t step = 8;
    for (size_t i = 0; i < encCount_; ++i) step = std::min(step, valueSize(enc_[i].type));
    const bool group = mode_ == SearchMode::Group;
    const size_t margin = group ? groupWindow_ : 0;
    const size_t w0 = valueSize(enc_[0].type);

    // 群組模式：其他數值的原始值
    uint64_t extraRaw[2] = {0, 0};
    for (int j = 0; j < groupExtraCount_; ++j) extraRaw[j] = encodeValue(enc_[0].type, groupExtra_[j]);

    uint64_t total = 0;
    for (size_t i = 0; i < nRegions_; ++i) total += regions_[i].size;
    p.total = total;
    p.done = 0;

    const int outSlot = started_ && !snapshot_ ? 1 - slot_ : 0;
    FILE *f = std::fopen(pathFor(outSlot).c_str(), "wb");
    if (!f) return SearchResult::IoError;
    BufWriter out(f, outBuf_, kIoBuf);
    uint64_t found = 0;
    SearchResult res = SearchResult::Ok;

    for (size_t r = 0; r < nRegions_ && res == SearchResult::Ok; ++r) {
        const Region &rg = regions_[r];
        const uint64_t rgEnd = rg.base + rg.size;
        for (uint64_t off = 0; off < rg.size; off += kChunk) {
            if (p.cancel) { res = SearchResult::Cancelled; break; }
            const uint64_t start = rg.base + off;
            const size_t n = (size_t)std::min<uint64_t>(kChunk, rg.size - off);
            p.done += n;
            // 群組模式前後多讀一點，邊界附近的數值才不會漏掉
            const uint64_t readStart = std::max<uint64_t>(rg.base, start >= margin ? start - margin : 0);
            const uint64_t readEnd = std::min<uint64_t>(rgEnd, start + n + margin);
            if (!mem.read(readStart, chunk_, (size_t)(readEnd - readStart))) continue;   // 讀不到的頁面直接跳過
            const uint8_t *base = chunk_ + (start - readStart);
            const size_t before = (size_t)(start - readStart), after = (size_t)(readEnd - start - n);

            for (size_t i = 0; i + step <= n; i += step) {
                for (size_t e = 0; e < encCount_; ++e) {
                    const size_t w = valueSize(enc_[e].type);
                    if ((i % w) != 0 || i + w > n) continue;
                    const uint64_t v = load(base + i, w);
                    if (!match(enc_[e], e, spec.cond, v, 0)) continue;

                    if (group) {
                        // 其他數值都要在前後 window 內出現（同樣對齊）
                        bool all = true;
                        for (int j = 0; j < groupExtraCount_ && all; ++j) {
                            bool hit = false;
                            const int64_t lo = -(int64_t)std::min<size_t>(margin, i + before);
                            const int64_t hi = (int64_t)std::min<size_t>(margin, n - i - w + after);
                            for (int64_t d = lo - (lo % (int64_t)w0); d <= hi && !hit; d += (int64_t)w0) {
                                if (d == 0) continue;
                                const int64_t pos = (int64_t)i + d;
                                if (pos < -(int64_t)before || pos + (int64_t)w0 > (int64_t)(n + after)) continue;
                                if (load(base + pos, w0) == extraRaw[j]) hit = true;
                            }
                            all = hit;
                        }
                        if (!all) continue;
                    }
                    if (++found > maxCandidates_) { res = SearchResult::TooMany; break; }
                    out.put({(start + i) | ((uint64_t)e << 56), v});
                    break;   // 同一個位址只記第一個對上的存法
                }
                if (res != SearchResult::Ok) break;
            }
            if (res != SearchResult::Ok) break;
        }
    }
    out.flush();
    std::fclose(f);

    if (res != SearchResult::Ok || !out.ok) {
        std::remove(pathFor(outSlot).c_str());
        return out.ok ? res : SearchResult::IoError;
    }
    if (started_ && !snapshot_) std::remove(pathFor(slot_).c_str());
    clearSnapshotFiles();
    snapshot_ = false;
    slot_ = outSlot;
    count_ = found;
    started_ = true;
    return SearchResult::Ok;
}

// 未知數值第一次：把要搜的記憶體原封不動存到 SD 卡
SearchResult SearchEngine::snapshotFirst(MemoryReader &mem, SearchProgress &p) {
    const size_t w = valueSize(enc_[0].type);
    uint64_t total = 0;
    for (size_t i = 0; i < nRegions_; ++i) total += regions_[i].size;
    const uint64_t need = total + total / w / 8 * 2 + (64ull << 20);   // 快照 + 兩份位元表 + 64MB 保留
    const uint64_t freeBytes = (freeSpace_ ? freeSpace_ : defaultFreeSpace)(workDir_);
    if (freeBytes < need) return SearchResult::NoSpace;

    p.total = total;
    p.done = 0;
    reset();
    FILE *f = std::fopen(file("snap.bin").c_str(), "wb");
    if (!f) return SearchResult::IoError;
    SearchResult res = SearchResult::Ok;
    for (size_t r = 0; r < nRegions_ && res == SearchResult::Ok; ++r) {
        for (uint64_t off = 0; off < regions_[r].size; off += kChunk) {
            if (p.cancel) { res = SearchResult::Cancelled; break; }
            const size_t n = (size_t)std::min<uint64_t>(kChunk, regions_[r].size - off);
            if (!mem.read(regions_[r].base + off, chunk_, n)) std::memset(chunk_, 0, n);
            if (std::fwrite(chunk_, 1, n, f) != n) { res = SearchResult::IoError; break; }
            p.done += n;
        }
    }
    std::fclose(f);
    if (res == SearchResult::Ok) {
        FILE *rf = std::fopen(file("regions.bin").c_str(), "wb");
        if (!rf) res = SearchResult::IoError;
        else {
            const uint64_t n = nRegions_;
            std::fwrite(&n, sizeof(n), 1, rf);
            std::fwrite(regions_, sizeof(Region), nRegions_, rf);
            std::fclose(rf);
        }
    }
    if (res != SearchResult::Ok) { clearSnapshotFiles(); return res; }
    snapshot_ = true;
    allAlive_ = true;
    snapBytes_ = total;
    count_ = total / w;
    started_ = true;
    return SearchResult::Ok;
}

// ───────────────────────── 接著搜尋 ─────────────────────────

SearchResult SearchEngine::nextScan(MemoryReader &mem, Cond cond, double value, SearchProgress &p) {
    if (!started_) return SearchResult::NotStarted;
    prepareTargets(cond, value);
    if (snapshot_) return snapshotNext(mem, cond, p);

    const size_t nRegions = mem.regions(regions_, kMaxRegions);
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
        const uint64_t addr = candAddr(c);
        const uint8_t ei = candEnc(c) < encCount_ ? candEnc(c) : 0;
        const size_t w = valueSize(enc_[ei].type);
        if (!(winLen && addr >= winBase && addr + w <= winBase + winLen)) {
            // 找出這個位址所在的區段，視窗不能跨出區段（跨出去會讀失敗）
            while (rIdx < nRegions && regions_[rIdx].base + regions_[rIdx].size <= addr) ++rIdx;
            if (rIdx >= nRegions || addr < regions_[rIdx].base) { winLen = 0; continue; }   // 這段記憶體已經不在了
            const uint64_t regEnd = regions_[rIdx].base + regions_[rIdx].size;
            winBase = addr;
            winLen = std::min<uint64_t>(kChunk, regEnd - addr);
            winOk = mem.read(winBase, chunk_, (size_t)winLen);
        }
        if (!winOk || addr + w > winBase + winLen) continue;
        const uint64_t v = load(chunk_ + (addr - winBase), w);
        if (!match(enc_[ei], ei, cond, v, c.value)) continue;
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

// 未知數值：拿現在的記憶體跟快照比，用位元表記錄哪些位置還「活著」。
// 剩下的少到放得進候選檔時，就轉成一般的候選清單。
SearchResult SearchEngine::snapshotNext(MemoryReader &mem, Cond cond, SearchProgress &p) {
    const Encoding &e = enc_[0];
    const size_t w = valueSize(e.type);
    p.total = snapBytes_;
    p.done = 0;

    FILE *snap = std::fopen(file("snap.bin").c_str(), "r+b");
    if (!snap) return SearchResult::IoError;
    const int outAlive = allAlive_ ? 0 : 1 - aliveSlot_;
    FILE *bitsIn = allAlive_ ? nullptr : std::fopen(file(aliveSlot_ ? "alive1.bin" : "alive0.bin").c_str(), "rb");
    FILE *bitsOut = std::fopen(file(outAlive ? "alive1.bin" : "alive0.bin").c_str(), "wb");
    FILE *cand = std::fopen(pathFor(0).c_str(), "wb");
    if ((!allAlive_ && !bitsIn) || !bitsOut || !cand) {
        std::fclose(snap);
        if (bitsIn) std::fclose(bitsIn);
        if (bitsOut) std::fclose(bitsOut);
        if (cand) std::fclose(cand);
        return SearchResult::IoError;
    }
    BufWriter out(cand, outBuf_, kIoBuf);
    bool candOk = true;   // 活著的超過上限就不再寫候選檔
    uint64_t alive = 0;
    uint64_t filePos = 0;
    SearchResult res = SearchResult::Ok;

    for (size_t r = 0; r < nRegions_ && res == SearchResult::Ok; ++r) {
        const Region &rg = regions_[r];
        for (uint64_t off = 0; off < rg.size; off += kChunk) {
            if (p.cancel) { res = SearchResult::Cancelled; break; }
            const size_t n = (size_t)std::min<uint64_t>(kChunk, rg.size - off);
            const size_t slots = n / w, bitBytes = (slots + 7) / 8;

            std::fseek(snap, (long)filePos, SEEK_SET);
            if (std::fread(snap_, 1, n, snap) != n) { res = SearchResult::IoError; break; }
            if (allAlive_) std::memset(bitsIn_, 0xFF, bitBytes);
            else if (std::fread(bitsIn_, 1, bitBytes, bitsIn) != bitBytes) { res = SearchResult::IoError; break; }
            std::memset(bitsOut_, 0, bitBytes);

            const bool ok = mem.read(rg.base + off, chunk_, n);
            if (ok) {
                for (size_t s = 0; s < slots; ++s) {
                    if (!(bitsIn_[s >> 3] & (1 << (s & 7)))) continue;
                    const uint64_t cur = load(chunk_ + s * w, w), prev = load(snap_ + s * w, w);
                    if (!match(e, 0, cond, cur, prev)) continue;
                    bitsOut_[s >> 3] |= (uint8_t)(1 << (s & 7));
                    ++alive;
                    if (candOk) {
                        if (alive > maxCandidates_) candOk = false;
                        else out.put({rg.base + off + s * w, cur});
                    }
                }
                // 現在的值寫回快照，下次「變大／變小」跟這次比
                std::fseek(snap, (long)filePos, SEEK_SET);
                if (std::fwrite(chunk_, 1, n, snap) != n) { res = SearchResult::IoError; break; }
            }
            if (std::fwrite(bitsOut_, 1, bitBytes, bitsOut) != bitBytes) { res = SearchResult::IoError; break; }
            filePos += n;
            p.done += n;
        }
    }
    out.flush();
    std::fclose(snap);
    if (bitsIn) std::fclose(bitsIn);
    std::fclose(bitsOut);
    std::fclose(cand);

    if (res != SearchResult::Ok || !out.ok) {
        // 快照已經部分寫回，這次的結果作廢；位元表沒換，下次重比會以快照當下為準
        std::remove(file(outAlive ? "alive1.bin" : "alive0.bin").c_str());
        std::remove(pathFor(0).c_str());
        return out.ok ? res : SearchResult::IoError;
    }
    if (!allAlive_) std::remove(file(aliveSlot_ ? "alive1.bin" : "alive0.bin").c_str());
    allAlive_ = false;
    aliveSlot_ = outAlive;
    count_ = alive;

    if (candOk) {
        // 剩下的夠少了：轉成一般候選清單，之後就跟一般搜尋一樣
        clearSnapshotFiles();
        snapshot_ = false;
        slot_ = 0;
    } else {
        std::remove(pathFor(0).c_str());
    }
    return SearchResult::Ok;
}

// ───────────────────────── 顯示、群組、存檔 ─────────────────────────

size_t SearchEngine::peek(Candidate *out, size_t max, uint64_t offset) {
    if (!started_ || snapshot_ || offset >= count_) return 0;
    FILE *f = std::fopen(pathFor(slot_).c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, (long)(offset * sizeof(Candidate)), SEEK_SET);
    size_t n = std::fread(out, sizeof(Candidate), max, f);
    std::fclose(f);
    return n;
}

int SearchEngine::groupNeighbors(MemoryReader &mem, uint64_t addr, int32_t *outOffsets) {
    if (mode_ != SearchMode::Group || groupExtraCount_ == 0) return 0;
    const size_t w = valueSize(enc_[0].type);
    const uint64_t start = addr >= groupWindow_ ? addr - groupWindow_ : 0;
    const size_t len = groupWindow_ * 2 + w;
    if (!mem.read(start, chunk_, len)) return 0;
    int found = 0;
    for (int j = 0; j < groupExtraCount_; ++j) {
        const uint64_t raw = encodeValue(enc_[0].type, groupExtra_[j]);
        int32_t best = 0;
        bool hit = false;
        for (size_t i = 0; i + w <= len; i += w) {
            const int32_t d = (int32_t)((int64_t)(start + i) - (int64_t)addr);
            if (d == 0 || load(chunk_ + i, w) != raw) continue;
            if (!hit || std::abs(d) < std::abs(best)) { best = d; hit = true; }
        }
        if (hit) outOffsets[found++] = best;
    }
    return found;
}

namespace {
struct SavedEngine {
    uint32_t magic;      // 'CTWS'
    uint32_t version;
    uint8_t mode, encCount, slot, snapshot, allAlive, aliveSlot, groupExtraCount, pad;
    Encoding enc[SearchEngine::kMaxEnc];
    double groupExtra[2];
    uint32_t groupWindow;
    uint32_t pad2;
    uint64_t count;
    uint64_t snapBytes;
};
constexpr uint32_t kMagic = 0x53575443, kVersion = 2;

long fileSize(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long s = std::ftell(f);
    std::fclose(f);
    return s;
}
}

bool SearchEngine::saveState(const std::string &path) const {
    if (!started_) { std::remove(path.c_str()); return true; }
    SavedEngine st{};
    st.magic = kMagic;
    st.version = kVersion;
    st.mode = (uint8_t)mode_;
    st.encCount = encCount_;
    st.slot = (uint8_t)slot_;
    st.snapshot = snapshot_;
    st.allAlive = allAlive_;
    st.aliveSlot = (uint8_t)aliveSlot_;
    st.groupExtraCount = (uint8_t)groupExtraCount_;
    std::memcpy(st.enc, enc_, sizeof(enc_));
    st.groupExtra[0] = groupExtra_[0];
    st.groupExtra[1] = groupExtra_[1];
    st.groupWindow = groupWindow_;
    st.count = count_;
    st.snapBytes = snapBytes_;
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
    if (!ok || st.magic != kMagic || st.version != kVersion || st.slot > 1 || st.encCount == 0 ||
        st.encCount > kMaxEnc || st.mode > (uint8_t)SearchMode::Unknown)
        return false;

    if (st.snapshot) {
        // 快照、區段表、位元表都要在
        if ((uint64_t)fileSize(file("snap.bin")) != st.snapBytes) return false;
        FILE *rf = std::fopen(file("regions.bin").c_str(), "rb");
        if (!rf) return false;
        uint64_t n = 0;
        const bool rok = std::fread(&n, sizeof(n), 1, rf) == 1 && n > 0 && n <= kMaxRegions &&
                         std::fread(regions_, sizeof(Region), (size_t)n, rf) == n;
        std::fclose(rf);
        if (!rok) return false;
        if (!st.allAlive && fileSize(file(st.aliveSlot ? "alive1.bin" : "alive0.bin")) < 0) return false;
        nRegions_ = (size_t)n;
    } else {
        // 候選檔要還在，而且大小剛好是筆數 × 16 bytes
        const long size = fileSize(pathFor(st.slot));
        if (size < 0 || (uint64_t)size != st.count * sizeof(Candidate)) return false;
    }

    mode_ = (SearchMode)st.mode;
    encCount_ = st.encCount;
    std::memcpy(enc_, st.enc, sizeof(enc_));
    slot_ = st.slot;
    snapshot_ = st.snapshot;
    allAlive_ = st.allAlive;
    aliveSlot_ = st.aliveSlot;
    groupExtraCount_ = std::min<int>(st.groupExtraCount, 2);
    groupExtra_[0] = st.groupExtra[0];
    groupExtra_[1] = st.groupExtra[1];
    groupWindow_ = std::min<uint32_t>(st.groupWindow, kMaxWindow);
    count_ = st.count;
    snapBytes_ = st.snapBytes;
    started_ = true;
    return true;
}

Cheat makeStaticWriteCheat(const std::string &name, uint8_t regionType, uint64_t offset,
                           ValueType t, uint64_t valueBits) {
    // 0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV)
    // T = 寬度、M = 記憶體區（0 主程式／1 heap／2 alias／3 aslr）、R = 偏移暫存器（不用）、A = 40-bit 偏移
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
