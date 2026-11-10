#include "cheat_core.hpp"
#include "gbk_table.hpp"
#include "s2t_table.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace cheattw {

namespace {

std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isHexWord(const std::string &w) {
    if (w.size() != 8) return false;
    return std::all_of(w.begin(), w.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

std::string readAll(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

CheatFile parseCheatText(const std::string &text) {
    CheatFile out;
    Cheat *cur = nullptr;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;

    while (std::getline(in, line)) {
        ++lineNo;
        // 去掉 UTF-8 BOM
        if (lineNo == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);
        line = trim(line);
        if (line.empty()) continue;

        const char open = line[0];
        if ((open == '[' || open == '{')) {
            const char close = (open == '[') ? ']' : '}';
            // 用最後一個括號，名稱裡可能有 [Hold R] 這種巢狀括號
            size_t end = line.rfind(close);
            if (end == 0) end = std::string::npos;
            if (end == std::string::npos) {
                out.warnings.push_back("第 " + std::to_string(lineNo) + " 行：名稱缺少結尾括號");
                end = line.size();
            }
            Cheat c;
            c.name = trim(line.substr(1, end - 1));
            c.isMaster = (open == '{');
            if (c.isMaster) {
                if (out.hasMaster()) {
                    out.warnings.push_back("第 " + std::to_string(lineNo) + " 行：重複的 master code，已忽略");
                    cur = nullptr;
                    continue;
                }
                out.cheats.insert(out.cheats.begin(), c);
                cur = &out.cheats.front();
            } else {
                out.cheats.push_back(c);
                cur = &out.cheats.back();
            }
            continue;
        }

        // 碼的內容
        std::istringstream words(line);
        std::string w;
        while (words >> w) {
            if (!isHexWord(w)) {
                out.warnings.push_back("第 " + std::to_string(lineNo) + " 行：無法辨識「" + w + "」");
                break;
            }
            if (!cur) {
                out.warnings.push_back("第 " + std::to_string(lineNo) + " 行：碼前面沒有名稱");
                break;
            }
            cur->opcodes.push_back(static_cast<uint32_t>(std::stoul(w, nullptr, 16)));
        }
    }

    // 去掉沒有內容的條目（常見於只有標題的分隔線，例如 [--- 戰鬥 ---]）
    out.cheats.erase(std::remove_if(out.cheats.begin(), out.cheats.end(),
        [](const Cheat &c) { return c.opcodes.empty() && !c.isMaster; }), out.cheats.end());
    return out;
}

CheatFile parseCheatFile(const std::string &path) {
    return parseCheatText(readAll(path));
}

std::string toCheatText(const CheatFile &file, bool useZhName) {
    std::string out;
    char buf[16];
    for (const auto &c : file.cheats) {
        const std::string &n = (useZhName && !c.zhName.empty()) ? c.zhName : c.name;
        out += c.isMaster ? "{" + n + "}\n" : "[" + n + "]\n";
        for (size_t i = 0; i < c.opcodes.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%08X", c.opcodes[i]);
            out += buf;
            // dmnt 解析時不看換行，這裡每 3 個字組換行只是方便人閱讀
            out += ((i + 1) % 3 == 0 || i + 1 == c.opcodes.size()) ? "\n" : " ";
        }
        out += "\n";
    }
    return out;
}

void Translator::loadTsvText(const std::string &text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        DictEntry e;
        std::istringstream cols(line);
        std::getline(cols, e.en, '\t');
        std::getline(cols, e.zh, '\t');
        std::getline(cols, e.category, '\t');
        e.en = trim(e.en);
        e.zh = trim(e.zh);
        e.category = trim(e.category);
        if (e.en.empty() || e.zh.empty()) continue;
        entries_.push_back(e);
    }
    std::stable_sort(entries_.begin(), entries_.end(),
        [](const DictEntry &a, const DictEntry &b) { return a.en.size() > b.en.size(); });
}

bool Translator::loadTsv(const std::string &path) {
    std::string t = readAll(path);
    if (t.empty()) return false;
    loadTsvText(t);
    return true;
}

std::string Translator::translate(const std::string &raw, std::string *outCategory) const {
    const std::string en = fixEncoding(raw);
    std::string result = en;
    std::string lowered = lower(en);
    std::string category;

    for (const auto &e : entries_) {
        const std::string key = lower(e.en);
        size_t pos = 0;
        while ((pos = lowered.find(key, pos)) != std::string::npos) {
            // 只比對完整單字，避免 "HP" 吃到 "SHIP"
            bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(lowered[pos - 1]));
            size_t endPos = pos + key.size();
            bool rightOk = endPos >= lowered.size() || !std::isalnum(static_cast<unsigned char>(lowered[endPos]));
            if (!leftOk || !rightOk) { pos = endPos; continue; }

            result.replace(pos, key.size(), e.zh);
            // 用不會再被比對到的佔位字元同步更新小寫副本
            lowered.replace(pos, key.size(), std::string(e.zh.size(), '\x01'));
            if (category.empty() && !e.category.empty()) category = e.category;
            pos += e.zh.size();
        }
    }

    // 中文之間多餘的空白收掉：「無限 血量」→「無限血量」
    std::string compact;
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i] == ' ' && i > 0 && i + 1 < result.size() &&
            (unsigned char)result[i - 1] >= 0x80 && (unsigned char)result[i + 1] >= 0x80)
            continue;
        compact += result[i];
    }

    if (outCategory) *outCategory = category.empty() ? "其他" : category;
    return toTraditional(compact);
}

void Translator::apply(Cheat &cheat) const {
    cheat.zhName = translate(cheat.name, &cheat.category);
}

void Translator::apply(CheatFile &file) const {
    for (auto &c : file.cheats) apply(c);
}

namespace {

// 解一個 UTF-8 字元；失敗回傳 false
bool decodeUtf8(const std::string &s, size_t &i, uint32_t &cp) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { cp = c; ++i; return true; }
    int n;
    if ((c >> 5) == 0x6) n = 1;
    else if ((c >> 4) == 0xE) n = 2;
    else if ((c >> 3) == 0x1E) n = 3;
    else return false;
    if (i + n >= s.size()) return false;   // 後面的位元組不夠
    cp = c & (0x3F >> n);
    for (int k = 1; k <= n; ++k) {
        const unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (cc & 0x3F);
    }
    // 過長編碼、代理區、超出範圍都算不合法
    static const uint32_t minCp[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < minCp[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += n + 1;
    return true;
}

void appendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

} // namespace

bool isValidUtf8(const std::string &s) {
    size_t i = 0;
    uint32_t cp;
    while (i < s.size())
        if (!decodeUtf8(s, i, cp)) return false;
    return true;
}

std::string fixEncoding(const std::string &s) {
    if (isValidUtf8(s)) return s;

    // 試著當 GBK 解：每個位元組不是 ASCII 就要跟下一個位元組組成合法的 GBK 字
    std::string out;
    bool ok = true;
    for (size_t i = 0; i < s.size() && ok;) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += (char)c; ++i; continue; }
        if (i + 1 >= s.size() || c < kGbkLead || c == 0xFF) { ok = false; break; }
        const unsigned char t = (unsigned char)s[i + 1];
        if (t < kGbkTrail || t == 0xFF) { ok = false; break; }
        const uint16_t cp = kGbk[(c - kGbkLead) * kGbkTrailCount + (t - kGbkTrail)];
        if (!cp) { ok = false; break; }
        appendUtf8(out, cp);
        i += 2;
    }
    if (ok) return out;

    // 都不是：保留合法的部分，壞掉的位元組丟掉
    out.clear();
    for (size_t i = 0; i < s.size();) {
        size_t j = i;
        uint32_t cp;
        if (decodeUtf8(s, j, cp)) { out.append(s, i, j - i); i = j; }
        else ++i;
    }
    return out;
}

std::string toTraditional(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const size_t start = i;
        uint32_t cp;
        if (!decodeUtf8(s, i, cp)) { ++i; continue; }
        if (cp < 0x2E80) { out.append(s, start, i - start); continue; }   // 不是中日韓文字，原樣保留
        size_t lo = 0, hi = sizeof(kS2T) / sizeof(kS2T[0]);
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (kS2T[mid].s < cp) lo = mid + 1; else hi = mid;
        }
        if (lo < sizeof(kS2T) / sizeof(kS2T[0]) && kS2T[lo].s == cp) appendUtf8(out, kS2T[lo].t);
        else out.append(s, start, i - start);
    }
    return out;
}

std::string buildIdToHex(const uint8_t *buildId) {
    char buf[17];
    for (int i = 0; i < 8; ++i) std::snprintf(buf + i * 2, 3, "%02X", buildId[i]);
    return std::string(buf, 16);
}

std::string titleIdToHex(uint64_t titleId) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(titleId));
    return buf;
}

} // namespace cheattw
