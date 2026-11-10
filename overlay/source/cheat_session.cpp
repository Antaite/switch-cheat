#include "cheat_session.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace cheattw {

CheatSession &session() {
    static CheatSession s;
    return s;
}

bool fileExists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

void mkdirs(const std::string &path) {
    for (size_t i = std::strlen("sdmc:/"); i < path.size(); ++i)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
    mkdir(path.c_str(), 0777);
}

namespace {

// 檔案複製用固定 4KB 緩衝（放 static，不佔 stack）
bool copyFile(const std::string &from, const std::string &to) {
    static char buf[0x1000];
    FILE *in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE *out = std::fopen(to.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
    std::fclose(in);
    std::fclose(out);
    return ok;
}

// 名稱塞進 0x40 bytes，切在 UTF-8 字元邊界上，避免中文被切一半
void copyName(char *dst, const std::string &src) {
    size_t n = std::min(src.size(), (size_t)DMNT_CHEAT_NAME_MAX - 1);
    while (n > 0 && n < src.size() && ((unsigned char)src[n] & 0xC0) == 0x80) --n;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

bool toDefinition(const Cheat &c, DmntCheatDefinition &def) {
    if (c.opcodes.empty() || c.opcodes.size() > DMNT_CHEAT_OPCODE_MAX) return false;
    std::memset(&def, 0, sizeof(def));
    copyName(def.readable_name, c.name);
    def.num_opcodes = (u32)c.opcodes.size();
    std::memcpy(def.opcodes, c.opcodes.data(), c.opcodes.size() * sizeof(u32));
    return true;
}

// DmntCheatDefinition 有 1KB 多，放 static 避免 overlay 的小 stack 爆掉
DmntCheatDefinition g_def;
DmntCheatEntry g_batch[16];

} // namespace

Result CheatSession::open() {
    if (open_) return 0;
    if (!dictLoaded_) {
        dict_.loadTsv(kDictPath);
        dictLoaded_ = true;
    }
    Result rc = dmntchtInitialize();
    if (R_FAILED(rc)) return rc;
    open_ = true;

    bool has = false;
    dmntchtHasCheatProcess(&has);
    if (!has) {
        dmntchtForceOpenCheatProcess();
        dmntchtHasCheatProcess(&has);
    }
    hasGame_ = has;
    if (!has) return 0;

    DmntCheatProcessMetadata meta{};
    rc = dmntchtGetCheatProcessMetadata(&meta);
    if (R_FAILED(rc)) { hasGame_ = false; return rc; }

    const std::string newTid = titleIdToHex(meta.title_id);
    const std::string newBid = buildIdToHex(meta.main_nso_build_id);
    if (newTid != tid_ || newBid != bid_) loadedGroup_.clear();   // 換遊戲了
    tid_ = newTid;
    bid_ = newBid;
    ext_.mainBase = meta.main_nso_extents.base;   ext_.mainSize = meta.main_nso_extents.size;
    ext_.heapBase = meta.heap_extents.base;       ext_.heapSize = meta.heap_extents.size;
    ext_.aliasBase = meta.alias_extents.base;     ext_.aliasSize = meta.alias_extents.size;
    ext_.spaceBase = meta.address_space_extents.base; ext_.spaceSize = meta.address_space_extents.size;
    return refresh();
}

void CheatSession::close() {
    if (!open_ || keepAlive_) return;
    dmntchtExit();
    open_ = false;
}

Result CheatSession::refresh() {
    cheats_.clear();
    bump();
    if (!open_ || !hasGame_) return 0;

    u64 count = 0;
    Result rc = dmntchtGetCheatCount(&count);
    if (R_FAILED(rc) || count == 0) return rc;
    cheats_.reserve(count);

    // 一次只拿 16 條（約 17KB），不管碼有多少條記憶體用量都固定
    for (u64 offset = 0; offset < count;) {
        u64 got = 0;
        rc = dmntchtGetCheats(g_batch, 16, offset, &got);
        if (R_FAILED(rc) || got == 0) break;
        for (u64 i = 0; i < got; ++i) {
            const auto &e = g_batch[i];
            LiveCheat lc;
            lc.id = e.cheat_id;
            lc.enabled = e.enabled;
            lc.isMaster = (e.cheat_id == 0);   // dmnt 固定把 master code 放在 id 0
            lc.name.assign(e.definition.readable_name, strnlen(e.definition.readable_name, DMNT_CHEAT_NAME_MAX));
            lc.zhName = dict_.translate(lc.name, &lc.category);
            cheats_.push_back(std::move(lc));
        }
        offset += got;
    }
    return rc;
}

Result CheatSession::toggle(u32 id, bool wantOn) {
    for (auto &c : cheats_) {
        if (c.id != id) continue;
        if (c.isMaster || c.enabled == wantOn) return 0;
        Result rc = dmntchtToggleCheat(id);
        if (R_SUCCEEDED(rc)) { c.enabled = wantOn; bump(); }
        return rc;
    }
    return 0;
}

void CheatSession::disableAll() {
    for (auto &c : cheats_)
        if (c.enabled && !c.isMaster && R_SUCCEEDED(dmntchtToggleCheat(c.id))) c.enabled = false;
    bump();
}

size_t CheatSession::enabledCount() const {
    return std::count_if(cheats_.begin(), cheats_.end(),
        [](const LiveCheat &c) { return c.enabled && !c.isMaster; });
}

size_t CheatSession::countInCategory(const std::string &cat) const {
    if (cat == "全部") return cheats_.size();
    return std::count_if(cheats_.begin(), cheats_.end(), [&](const LiveCheat &c) { return c.category == cat; });
}

std::vector<DbGroup> CheatSession::dbGroups() const {
    std::vector<DbGroup> out;
    if (!hasGame_) return out;
    const std::string dir = std::string(kDbDir) + "/" + tid_ + "/";

    // .meta.tsv：檔名<TAB>條數<TAB>要修補檔<TAB>來源
    FILE *f = std::fopen((dir + bid_ + ".meta.tsv").c_str(), "r");
    if (f) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            char *cols[4] = {line, nullptr, nullptr, nullptr};
            for (int i = 1; i < 4; ++i) {
                cols[i] = cols[i - 1] ? std::strchr(cols[i - 1], '\t') : nullptr;
                if (cols[i]) *cols[i]++ = '\0';
            }
            if (!cols[1]) continue;
            DbGroup g;
            g.file = cols[0];
            g.count = std::atoi(cols[1]);
            g.needsPatch = cols[2] && cols[2][0] == '1';
            if (cols[3]) { g.sources = cols[3]; while (!g.sources.empty() && (g.sources.back() == '\n' || g.sources.back() == '\r')) g.sources.pop_back(); }
            if (fileExists(dir + g.file)) out.push_back(std::move(g));
        }
        std::fclose(f);
    }
    // 沒有 meta 檔（例如自己手動放的）也能用
    if (out.empty() && fileExists(dir + bid_ + ".txt")) {
        DbGroup g;
        g.file = bid_ + ".txt";
        g.count = (int)parseCheatFile(dir + g.file).cheats.size();
        out.push_back(g);
    }
    return out;
}

Result CheatSession::loadGroup(const DbGroup &g, std::string &outMessage) {
    const std::string path = std::string(kDbDir) + "/" + tid_ + "/" + g.file;
    Result rc = loadFromFile(path, outMessage);
    if (R_SUCCEEDED(rc)) {
        loadedGroup_ = g.file;
        if (g.needsPatch) {
            int n = installPatches(g);
            outMessage += n > 0 ? "，已安裝修補檔（重開遊戲後生效）" : "，修補檔安裝失敗";
        }
    }
    return rc;
}

int CheatSession::installPatches(const DbGroup &g) {
    // db/{TID}/patches/{來源}/*.ips → atmosphere/exefs_patches/cheattw_{來源}/
    int installed = 0;
    std::string srcs = g.sources;
    size_t start = 0;
    while (start <= srcs.size()) {
        size_t comma = srcs.find(',', start);
        std::string sid = srcs.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? srcs.size() + 1 : comma + 1;
        if (sid.empty()) continue;

        const std::string from = std::string(kDbDir) + "/" + tid_ + "/patches/" + sid;
        DIR *d = opendir(from.c_str());
        if (!d) continue;
        const std::string to = "sdmc:/atmosphere/exefs_patches/cheattw_" + sid;
        mkdirs(to);
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ips") == 0 && n.compare(0, 16, bid_) == 0)
                if (copyFile(from + "/" + n, to + "/" + n)) ++installed;
        }
        closedir(d);
    }
    return installed;
}

Result CheatSession::loadFromFile(const std::string &path, std::string &outMessage) {
    if (!open_ || !hasGame_) { outMessage = "沒有執行中的遊戲"; return MAKERESULT(Module_Libnx, LibnxError_NotFound); }

    CheatFile file = parseCheatFile(path);
    if (file.cheats.empty()) { outMessage = "碼庫檔是空的或格式錯誤"; return MAKERESULT(Module_Libnx, LibnxError_BadInput); }

    // 1. 拿掉目前的碼；新檔沒有 master 時，舊的 master 也要拿掉
    for (const auto &c : cheats_)
        if (!c.isMaster || !file.hasMaster()) dmntchtRemoveCheat(c.id);

    // 2. 載入新的碼，全部預設關閉，讓使用者自己開
    size_t added = 0, skipped = 0;
    for (const auto &c : file.cheats) {
        if (!toDefinition(c, g_def)) { ++skipped; continue; }
        Result rc;
        if (c.isMaster) {
            rc = dmntchtSetMasterCheat(&g_def);
        } else {
            u32 newId = 0;
            rc = dmntchtAddCheat(&g_def, false, &newId);
        }
        if (R_SUCCEEDED(rc)) ++added; else ++skipped;
    }

    // 3. 寫到大氣層路徑（舊檔先備份成 .bak），下次開遊戲 dmnt 會自己載入
    const std::string dir = "sdmc:/atmosphere/contents/" + tid_ + "/cheats";
    const std::string target = dir + "/" + bid_ + ".txt";
    mkdirs(dir);
    if (fileExists(target) && !fileExists(target + ".bak")) copyFile(target, target + ".bak");   // 只備份最原始的那份
    copyFile(path, target);

    refresh();
    outMessage = "已載入 " + std::to_string(added) + " 條";
    if (skipped) outMessage += "，略過 " + std::to_string(skipped) + " 條";
    return 0;
}

Result CheatSession::addOwnCheat(const Cheat &c, bool enabled, std::string &outMessage) {
    if (!open_ || !hasGame_) { outMessage = "沒有執行中的遊戲"; return MAKERESULT(Module_Libnx, LibnxError_NotFound); }
    if (!toDefinition(c, g_def)) { outMessage = "碼太長"; return MAKERESULT(Module_Libnx, LibnxError_BadInput); }
    u32 id = 0;
    Result rc = dmntchtAddCheat(&g_def, enabled, &id);
    if (R_FAILED(rc)) { outMessage = "加入失敗（dmnt 可能已滿 128 條）"; return rc; }

    // 附加到大氣層的金手指檔，下次開遊戲還在
    const std::string dir = "sdmc:/atmosphere/contents/" + tid_ + "/cheats";
    mkdirs(dir);
    if (FILE *f = std::fopen((dir + "/" + bid_ + ".txt").c_str(), "a")) {
        CheatFile one;
        one.cheats.push_back(c);
        const std::string text = "\n" + toCheatText(one, false);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    refresh();
    outMessage = "已加入並開啟";
    return 0;
}

} // namespace cheattw
