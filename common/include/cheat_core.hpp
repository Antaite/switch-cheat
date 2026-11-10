// 金手指檔解析 + 中文翻譯（不依賴 libnx，可在電腦上單元測試）
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cheattw {

struct Cheat {
    std::string name;            // 原始名稱
    std::string zhName;          // 翻譯後名稱
    std::string category;        // 戰鬥／金錢／道具／畫面／其他
    bool isMaster = false;       // {} 大括號 = master code
    std::vector<uint32_t> opcodes;
};

struct CheatFile {
    std::vector<Cheat> cheats;   // master code（若有）一定放在第一個
    std::vector<std::string> warnings;
    bool hasMaster() const { return !cheats.empty() && cheats[0].isMaster; }
};

// 解析大氣層格式：{master} / [名稱] 後接 8 位十六進位字組
CheatFile parseCheatText(const std::string &text);
CheatFile parseCheatFile(const std::string &path);

// 序列化回大氣層格式
std::string toCheatText(const CheatFile &file, bool useZhName);

struct DictEntry {
    std::string en;        // 英文關鍵字（比對不分大小寫）
    std::string zh;        // 中文
    std::string category;  // 可空白
};

class Translator {
public:
    // TSV 格式：英文<TAB>中文<TAB>分類，# 開頭為註解
    bool loadTsv(const std::string &path);
    void loadTsvText(const std::string &text);
    void apply(Cheat &cheat) const;
    void apply(CheatFile &file) const;
    std::string translate(const std::string &en, std::string *outCategory = nullptr) const;
    size_t size() const { return entries_.size(); }
private:
    std::vector<DictEntry> entries_; // 依英文長度由長到短排序，長詞優先
};

// 文字處理
// fixEncoding：不是合法 UTF-8 時，當成 GBK（簡體中文常見的舊編碼）轉成 UTF-8；還是不行就去掉壞掉的位元組
std::string fixEncoding(const std::string &s);
// toTraditional：簡體字轉台灣繁體（OpenCC 單字表）
std::string toTraditional(const std::string &s);
bool isValidUtf8(const std::string &s);

// main_nso_build_id 前 8 bytes 轉成 16 碼大寫十六進位（大氣層檔名用）
std::string buildIdToHex(const uint8_t *buildId);
std::string titleIdToHex(uint64_t titleId);

} // namespace cheattw
