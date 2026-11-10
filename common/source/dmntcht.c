#include "dmntcht.h"
#include <string.h>

static Service g_dmntchtSrv;
static int g_refCnt;

Result dmntchtInitialize(void) {
    if (g_refCnt++ > 0) return 0;
    Result rc = smGetService(&g_dmntchtSrv, "dmnt:cht");
    if (R_FAILED(rc)) g_refCnt = 0;
    return rc;
}

void dmntchtExit(void) {
    if (g_refCnt > 0 && --g_refCnt == 0) serviceClose(&g_dmntchtSrv);
}

Service *dmntchtGetServiceSession(void) { return &g_dmntchtSrv; }

Result dmntchtHasCheatProcess(bool *out) {
    u8 tmp = 0;
    Result rc = serviceDispatchOut(&g_dmntchtSrv, 65000, tmp);
    if (R_SUCCEEDED(rc) && out) *out = tmp & 1;
    return rc;
}

Result dmntchtGetCheatProcessMetadata(DmntCheatProcessMetadata *out) {
    return serviceDispatchOut(&g_dmntchtSrv, 65002, *out);
}

Result dmntchtForceOpenCheatProcess(void) { return serviceDispatch(&g_dmntchtSrv, 65003); }
Result dmntchtPauseCheatProcess(void)     { return serviceDispatch(&g_dmntchtSrv, 65004); }
Result dmntchtResumeCheatProcess(void)    { return serviceDispatch(&g_dmntchtSrv, 65005); }

Result dmntchtReadCheatProcessMemory(u64 address, void *buffer, size_t size) {
    const struct { u64 address; u64 size; } in = { address, size };
    return serviceDispatchIn(&g_dmntchtSrv, 65102, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, size } });
}

Result dmntchtWriteCheatProcessMemory(u64 address, const void *buffer, size_t size) {
    const struct { u64 address; u64 size; } in = { address, size };
    return serviceDispatchIn(&g_dmntchtSrv, 65103, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { buffer, size } });
}

Result dmntchtQueryCheatProcessMemory(MemoryInfo *out, u64 address) {
    return serviceDispatchInOut(&g_dmntchtSrv, 65104, address, *out);
}

Result dmntchtGetCheatCount(u64 *out) {
    return serviceDispatchOut(&g_dmntchtSrv, 65200, *out);
}

Result dmntchtGetCheats(DmntCheatEntry *buffer, u64 max_count, u64 offset, u64 *out_count) {
    return serviceDispatchInOut(&g_dmntchtSrv, 65201, offset, *out_count,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, max_count * sizeof(*buffer) } });
}

Result dmntchtGetCheatById(DmntCheatEntry *out, u32 cheat_id) {
    return serviceDispatchIn(&g_dmntchtSrv, 65202, cheat_id,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out, sizeof(*out) } });
}

Result dmntchtToggleCheat(u32 cheat_id) {
    return serviceDispatchIn(&g_dmntchtSrv, 65203, cheat_id);
}

Result dmntchtAddCheat(const DmntCheatDefinition *def, bool enabled, u32 *out_id) {
    const u8 in = enabled != 0;
    return serviceDispatchInOut(&g_dmntchtSrv, 65204, in, *out_id,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { def, sizeof(*def) } });
}

Result dmntchtRemoveCheat(u32 cheat_id) {
    return serviceDispatchIn(&g_dmntchtSrv, 65205, cheat_id);
}

Result dmntchtSetMasterCheat(const DmntCheatDefinition *def) {
    return serviceDispatch(&g_dmntchtSrv, 65209,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { def, sizeof(*def) } });
}
