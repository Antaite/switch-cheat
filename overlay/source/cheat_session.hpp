// 跟 dmnt:cht 溝通的狀態管理：連線、讀遊戲資訊、讀／開關／載入金手指、碼庫組別、修補檔
#pragma once
#include <string>
#include <vector>
#include <switch.h>
#include "dmntcht.h"
#include "cheat_core.hpp"

namespace cheattw {

constexpr const char *kAppDir     = "sdmc:/switch/CheatTW";
constexpr const char *kDictPath   = "sdmc:/switch/CheatTW/zh_TW.tsv";
constexpr const char *kDbDir      = "sdmc:/switch/CheatTW/db";       // 碼庫快取：db/{TID}/{BID}.txt、.meta.tsv
constexpr const char *kSearchDir  = "sdmc:/switch/CheatTW/search";   // 進階搜尋的候選檔

struct LiveCheat {
    u32 id;
    bool enabled;
    bool isMaster;
    std::string name;      // dmnt 裡的原始名稱
    std::string zhName;
    std::string category;
};

// 碼庫裡同一個版本的一組碼（master code 不同就分組）
struct DbGroup {
    std::string file;      // 例：1A2B3C4D5E6F7081.alt1.txt
    int count = 0;
    bool needsPatch = false;
    std::string sources;   // 逗號分隔
};

struct Extents {
    u64 mainBase = 0, mainSize = 0;
    u64 heapBase = 0, heapSize = 0;
    u64 aliasBase = 0, aliasSize = 0;
    u64 spaceBase = 0, spaceSize = 0;
};

class CheatSession {
public:
    // dmnt 同時只服務兩個 session：overlay 顯示時才連線，隱藏時就放掉
    Result open();
    void close();
    bool isOpen() const { return open_; }
    // 進階搜尋跑在背景時，overlay 隱藏也不能斷線
    void setKeepAlive(bool k) { keepAlive_ = k; }
    bool keepAlive() const { return keepAlive_; }

    bool hasGame() const { return hasGame_; }
    const std::string &tid() const { return tid_; }
    const std::string &bid() const { return bid_; }
    // 線上碼庫下載的遊戲中文名（db/{TID}/title.txt），沒有就是空字串
    std::string gameTitle() const;
    const Extents &extents() const { return ext_; }

    // 每次碼有變動就 +1，畫面看到數字變了才重畫，不用每幀重算
    u32 generation() const { return gen_; }
    // 碼的清單本身換掉（載入新組合、重新讀取）才 +1；只是開關不算
    u32 structureGeneration() const { return structureGen_; }
    Result lastError() const { return lastError_; }

    Result refresh();
    Result toggle(u32 id, bool wantOn);
    void disableAll();
    const std::vector<LiveCheat> &cheats() const { return cheats_; }
    size_t enabledCount() const;
    size_t countInCategory(const std::string &cat) const;

    // 碼庫
    std::vector<DbGroup> dbGroups() const;
    Result loadGroup(const DbGroup &g, std::string &outMessage);
    std::string loadedGroup() const { return loadedGroup_; }

    // 進階搜尋做出來的碼：加進 dmnt 並存檔
    Result addOwnCheat(const Cheat &c, bool enabled, std::string &outMessage);

    Translator &translator() { return dict_; }

private:
    Result loadFromFile(const std::string &path, std::string &outMessage);
    int installPatches(const DbGroup &g);
    void bump() { ++gen_; }

    bool open_ = false;
    bool keepAlive_ = false;
    bool hasGame_ = false;
    std::string tid_, bid_;
    Extents ext_;
    std::vector<LiveCheat> cheats_;
    std::string loadedGroup_;
    Translator dict_;
    bool dictLoaded_ = false;
    u32 gen_ = 1;
    u32 structureGen_ = 1;
    Result lastError_ = 0;
};

CheatSession &session();

// 共用小工具
bool fileExists(const std::string &p);
void mkdirs(const std::string &path);

} // namespace cheattw
