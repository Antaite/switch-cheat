// dmnt:cht 服務的 IPC 包裝（大氣層內建金手指引擎）
// 命令編號對照 Atmosphere stratosphere/dmnt 的 dmnt_cheat_service
#pragma once
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u64 base;
    u64 size;
} DmntMemoryRegionExtents;

typedef struct {
    u64 process_id;
    u64 title_id;
    DmntMemoryRegionExtents main_nso_extents;
    DmntMemoryRegionExtents heap_extents;
    DmntMemoryRegionExtents alias_extents;
    DmntMemoryRegionExtents address_space_extents;
    u8 main_nso_build_id[0x20];
} DmntCheatProcessMetadata;

#define DMNT_CHEAT_NAME_MAX   0x40
#define DMNT_CHEAT_OPCODE_MAX 0x100

typedef struct {
    char readable_name[DMNT_CHEAT_NAME_MAX];
    u32 num_opcodes;
    u32 opcodes[DMNT_CHEAT_OPCODE_MAX];
} DmntCheatDefinition;

typedef struct {
    bool enabled;
    u32 cheat_id;
    DmntCheatDefinition definition;
} DmntCheatEntry;

Result dmntchtInitialize(void);
void   dmntchtExit(void);
Service *dmntchtGetServiceSession(void);

// 遊戲程序
Result dmntchtHasCheatProcess(bool *out);
Result dmntchtGetCheatProcessMetadata(DmntCheatProcessMetadata *out);
Result dmntchtForceOpenCheatProcess(void);
Result dmntchtPauseCheatProcess(void);
Result dmntchtResumeCheatProcess(void);

// 記憶體（進階模式用）
Result dmntchtGetCheatProcessMappingCount(u64 *out_count);
Result dmntchtGetCheatProcessMappings(MemoryInfo *buffer, u64 max_count, u64 offset, u64 *out_count);
Result dmntchtReadCheatProcessMemory(u64 address, void *buffer, size_t size);
Result dmntchtWriteCheatProcessMemory(u64 address, const void *buffer, size_t size);
Result dmntchtQueryCheatProcessMemory(MemoryInfo *out, u64 address);

// 金手指
Result dmntchtGetCheatCount(u64 *out);
Result dmntchtGetCheats(DmntCheatEntry *buffer, u64 max_count, u64 offset, u64 *out_count);
Result dmntchtGetCheatById(DmntCheatEntry *out, u32 cheat_id);
Result dmntchtToggleCheat(u32 cheat_id);
Result dmntchtAddCheat(const DmntCheatDefinition *def, bool enabled, u32 *out_id);
Result dmntchtRemoveCheat(u32 cheat_id);
Result dmntchtSetMasterCheat(const DmntCheatDefinition *def);

#ifdef __cplusplus
}
#endif
