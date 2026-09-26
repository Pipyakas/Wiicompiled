#include "hle_stubs.h"
#include "runtime_log.h"
#include <cstdint>
#include <cstdio>

extern "C" uint32_t Stub_8002001C(uint32_t ctx, int code)
{
    RT_LOGF(RT_TAG_HLE, "Stub_8002001C(0x%08x, %d)\n", ctx, code);
    (void)ctx;
    (void)code;
    return 0;
}

PPC_NATIVE_OVERRIDE(8002001C, Stub_8002001C, uint32_t, (uint32_t ctx, int code), (ctx, code));

// USA DOL has real prologues at 0x80020684 / 0x800206C0 / 0x800207F0 that the
// PAL MAP does not label (MAP ends TRKTargetSupportRequest at 0x80020638).
// OS::IsTitleInstalled (mis-mapped ctor walk) bctrl's 0x80020684 via _ctors
// table at 0x80244D40. Stub the TRK target-support helpers so boot continues.
extern "C" uint32_t TRKTargetSupportRequest_HLE_80020684()
{
    RT_LOGF(RT_TAG_HLE, "TRKTargetSupportRequest_HLE_80020684 (USA stub)\n");
    return 0;
}
REGISTER_NATIVE_FUNCTION(0x80020684, TRKTargetSupportRequest_HLE_80020684); // USA

extern "C" uint32_t TRKTargetSupportAux_HLE_800206C0()
{
    RT_LOGF(RT_TAG_HLE, "TRKTargetSupportAux_HLE_800206C0 (USA stub)\n");
    return 0;
}
REGISTER_NATIVE_FUNCTION(0x800206C0, TRKTargetSupportAux_HLE_800206C0); // USA

extern "C" uint32_t TRKTargetSupportAux2_HLE_800207F0()
{
    RT_LOGF(RT_TAG_HLE, "TRKTargetSupportAux2_HLE_800207F0 (USA stub)\n");
    return 0;
}
REGISTER_NATIVE_FUNCTION(0x800207F0, TRKTargetSupportAux2_HLE_800207F0); // USA
