// Intentionally empty of the original forwarders: the USA (RMCE01) translation
// tree now provides the real bodies for every symbol this file used to forward
// (func_8012B830, func_801A0620, func_801A1ED8, func_801A961C, func_801AADE0,
// func_801D8D30, func_801D9E94). Keeping the forwarders caused duplicate-symbol
// link errors against the USA shards.
//
// USA DOL has real prologues the PAL MAP (projects/mkwii-usa/MAP.txt is a PAL
// placeholder) does not label as separate entries. Boot hits them via
// InvokeIndirectCpu (vtable/bctrl) and fatals on "not translated".
// Register HLE shims here for those missing USA entries only.

#include "hle_stubs.h"
#include "runtime_log.h"
#include "memory.h"
#include <cstdint>

// 0x80009154 — real USA stwu prologue between EGG::TSystem::Initialize
// (MAP 80008fb4, ends ~8000914c) and MAP's InitRenderMode (80009190).
// Caller: RKSystem::WPADFree+0x3C (0x80008EEC) bctrl via object vtable.
// Body does multiple virtual calls + heap allocs (0x88 / 0x154 / 0x48);
// for now stub returns success so boot can advance past the missing entry.
extern "C" uint32_t EGG_SystemMethod_80009154_HLE(uint32_t thisPtr)
{
    RT_LOGF(RT_TAG_HLE, "EGG_SystemMethod_80009154_HLE(this=0x%08X) stub return 0\n", thisPtr);
    (void)thisPtr;
    return 0;
}
REGISTER_NATIVE_FUNCTION(0x80009154, EGG_SystemMethod_80009154_HLE); // USA

// 0x80008F6C — leaf getter, no prologue (MAP folds it into RKSystem::Main):
//   lwz r3, 0x64(r3); blr
// Caller: RKSystem::Main+0x10 (0x80008F00) bctrl. Returns *(this + 0x64).
extern "C" uint32_t RKSystem_Getter_80008F6C_HLE(uint32_t thisPtr)
{
    const uint32_t value = MemoryInline::FlatRead32(thisPtr + 0x64);
    RT_LOGF(RT_TAG_HLE, "RKSystem_Getter_80008F6C_HLE(this=0x%08X) -> 0x%08X\n", thisPtr, value);
    return value;
}
REGISTER_NATIVE_FUNCTION(0x80008F6C, RKSystem_Getter_80008F6C_HLE); // USA

// 0x80008F74 — real USA stwu prologue immediately after the leaf getter;
// also absent from the PAL MAP (between Main and GetSystemHeap@80008fac).
// Conservative: log + return 0 until a full USA map lands.
extern "C" uint32_t RKSystem_Method_80008F74_HLE(uint32_t thisPtr)
{
    RT_LOGF(RT_TAG_HLE, "RKSystem_Method_80008F74_HLE(this=0x%08X) stub return 0\n", thisPtr);
    (void)thisPtr;
    return 0;
}
REGISTER_NATIVE_FUNCTION(0x80008F74, RKSystem_Method_80008F74_HLE); // USA
