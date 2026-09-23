// USA (RMCE01) translation entry points sit a few instructions earlier than the
// PAL addresses the HLE sources still name. Provide the PAL-named symbols by
// forwarding to the USA translated bodies so the link succeeds and HLE
// fall-through paths execute the correct USA code.

#include "ppc_runtime.h"

extern "C" void func_801D8D1C(CpuContext* ctx);
extern "C" void func_801D9E90(CpuContext* ctx);
extern "C" void func_8012B790(CpuContext* ctx);
extern "C" void func_801A0580(CpuContext* ctx);
extern "C" void func_801AAD40(CpuContext* ctx);
extern "C" void func_801A957C(CpuContext* ctx);
extern "C" void func_801A1EB8(CpuContext* ctx);

extern "C" void func_801D8D30(CpuContext* ctx) { func_801D8D1C(ctx); }
extern "C" void func_801D9E94(CpuContext* ctx) { func_801D9E90(ctx); }
extern "C" void func_8012B830(CpuContext* ctx) { func_8012B790(ctx); }
extern "C" void func_801A0620(CpuContext* ctx) { func_801A0580(ctx); }
extern "C" void func_801AADE0(CpuContext* ctx) { func_801AAD40(ctx); }
extern "C" void func_801A961C(CpuContext* ctx) { func_801A957C(ctx); }
extern "C" void func_801A1ED8(CpuContext* ctx) { func_801A1EB8(ctx); }
