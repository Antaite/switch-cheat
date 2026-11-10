// 電腦上的單元測試：g++ -std=c++17 -I../include test_core.cpp ../source/cheat_core.cpp
#include "cheat_core.hpp"
#include <cassert>
#include <cstdio>

using namespace cheattw;

int main(int argc, char **argv) {
    const char *dict = argc > 1 ? argv[1] : "../../data/translations/zh_TW.tsv";
    const std::string sample =
        "\xEF\xBB\xBF[Infinite HP]\n"
        "040A0000 01234567 0000270F\n"
        "\n"
        "{Master Code}\n"
        "580F0000 01B2C3D4\n"
        "[--- Section ---]\n"
        "[Moon Jump [Hold R]]\n"
        "80000080\n"
        "400F0000 00000000 3F800000\n"
        "20000000\n"
        "[Max Money]\n"
        "040A0000 0ABCDEF0 05F5E0FF\n"
        "[Ship Speed]\n"
        "040A0000 00000010 00000002\n";

    CheatFile f = parseCheatText(sample);
    assert(f.hasMaster());
    assert(f.cheats.size() == 5);           // master + 4（分隔線被移除）
    assert(f.cheats[1].name == "Infinite HP");
    assert(f.cheats[1].opcodes.size() == 3);
    assert(f.cheats[1].opcodes[2] == 0x0000270F);

    Translator t;
    assert(t.loadTsv(dict));
    t.apply(f);
    for (const auto &c : f.cheats)
        std::printf("%-22s → %-24s [%s] %zu 字組\n", c.name.c_str(), c.zhName.c_str(), c.category.c_str(), c.opcodes.size());

    assert(f.cheats[0].zhName == "主碼");
    assert(f.cheats[1].zhName == "無限血量" && f.cheats[1].category == "戰鬥");
    assert(f.cheats[2].zhName == "月球跳 [按住 R]" && f.cheats[2].category == "移動");
    assert(f.cheats[3].zhName == "金錢最大" && f.cheats[3].category == "金錢");
    assert(f.cheats[4].zhName == "Ship 速度");    // SHIP 裡的 HP 不能被翻成血量

    // 序列化再解析，內容要一致
    CheatFile g = parseCheatText(toCheatText(f, false));
    assert(g.cheats.size() == f.cheats.size());
    for (size_t i = 0; i < g.cheats.size(); ++i) assert(g.cheats[i].opcodes == f.cheats[i].opcodes);

    assert(t.translate("60 FPS") == "60FPS 解鎖");   // 翻好的字不能再被短詞吃一次

    // 簡轉繁、GBK 亂碼修正、英文單字翻譯
    assert(toTraditional("无限血量 旧的会失效") == "無限血量 舊的會失效");
    assert(toTraditional("Infinite HP 無限") == "Infinite HP 無限");          // 繁體、英文不動
    const std::string gbk = "\xCE\xDE\xCF\xDE\xBD\xF0\xC7\xAE";            // GBK 的「无限金钱」
    assert(!isValidUtf8(gbk));
    assert(fixEncoding(gbk) == "无限金钱");
    assert(t.translate(gbk) == "無限金錢");
    assert(fixEncoding("ok\xFF") == "ok");                                   // 救不回來就去掉壞位元組
    assert(t.translate("Higher Attack") == "攻擊力提高");
    assert(t.translate("*无限Hp Jericos") == "*無限血量 Jericos");
    std::string cat;
    t.translate("Buy Ticket to get Gold", &cat);
    std::printf("Buy Ticket to get Gold → %s [%s]\n", t.translate("Buy Ticket to get Gold").c_str(), cat.c_str());

    uint8_t bid[0x20] = {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F, 0x70, 0x81};
    assert(buildIdToHex(bid) == "1A2B3C4D5E6F7081");
    assert(titleIdToHex(0x0100000000010000ULL) == "0100000000010000");

    std::printf("全部測試通過\n");
    return 0;
}
