#include "gx_internal.h"
#include "runtime_log.h"

extern "C" {
int g_gxFrameCount = 0;

int32_t g_scissorLeft = 0;
int32_t g_scissorTop = 0;
int32_t g_scissorWidth = 0;
int32_t g_scissorHeight = 0;

float g_viewportState[6] = {0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
float g_projectionVector[7] = {1.f, 1.f, 0.f, 1.f, 0.f, -1.f, 0.f};
}

bool g_alphaCompareValid = false;

TexCopyState g_texCopyState;

// Temporary GX submission counters (declared in gx_internal.h).
std::atomic<uint64_t> g_diagGxBeginCount{0};
std::atomic<uint64_t> g_diagGxEndCount{0};
std::atomic<uint64_t> g_diagCallListCount{0};
std::atomic<uint64_t> g_diagFifoByteCount{0};
std::atomic<uint64_t> g_diagDlBeginCount{0};
std::atomic<uint64_t> g_diagDlEndCount{0};
std::atomic<uint64_t> g_diagFifoDrawOpcode{0};
std::atomic<uint64_t> g_diagFifoRawOk{0};
std::atomic<uint64_t> g_diagFifoRawFail{0};
std::atomic<uint64_t> g_diagFifoIncrBegin{0};
std::atomic<uint64_t> g_diagFifoNullReset{0};
std::atomic<uint64_t> g_diagFifoUnknownByte{0};
std::atomic<uint64_t> g_diagFifoBpPkts{0};
std::atomic<uint64_t> g_diagFifoCpPkts{0};
std::atomic<uint64_t> g_diagFifoXfPkts{0};
std::atomic<uint64_t> g_diagFifoStallNop{0};
std::atomic<uint64_t> g_diagFifoStallBp{0};
std::atomic<uint64_t> g_diagFifoStallCp{0};
std::atomic<uint64_t> g_diagFifoStallXf{0};
std::atomic<uint64_t> g_diagFifoStallIndx{0};
std::atomic<uint64_t> g_diagFifoStallCallDl{0};
std::atomic<uint64_t> g_diagFifoStallDraw{0};
std::atomic<uint64_t> g_diagFifoStallAttr{0};
std::atomic<uint64_t> g_diagFifoStallBpReg{0xFFFFFFFFull};
std::atomic<uint64_t> g_diagFifoStallBpBacklog{0};

extern "C" void GX_HLE_DiagSnapshot(uint64_t* begins, uint64_t* ends, uint64_t* callLists,
                                    uint64_t* fifoBytes, uint64_t* dlBegins, uint64_t* dlEnds,
                                    uint64_t* dlActive) {
    if (begins) *begins = g_diagGxBeginCount.load(std::memory_order_relaxed);
    if (ends) *ends = g_diagGxEndCount.load(std::memory_order_relaxed);
    if (callLists) *callLists = g_diagCallListCount.load(std::memory_order_relaxed);
    if (fifoBytes) *fifoBytes = g_diagFifoByteCount.load(std::memory_order_relaxed);
    if (dlBegins) *dlBegins = g_diagDlBeginCount.load(std::memory_order_relaxed);
    if (dlEnds) *dlEnds = g_diagDlEndCount.load(std::memory_order_relaxed);
    if (dlActive) *dlActive = IsDisplayListActive() ? 1u : 0u;
}

extern "C" void GX_HLE_DiagFifoSnapshot(uint64_t* outDraw, uint64_t* outRawOk, uint64_t* outRawFail,
                                        uint64_t* outIncr, uint64_t* outNull, uint64_t* outUnk,
                                        uint64_t* outBp, uint64_t* outCp, uint64_t* outXf,
                                        uint64_t* outInBegin, uint64_t* outVertsRem,
                                        uint64_t* outBacklog, uint64_t* outNAttr) {
    if (outDraw) *outDraw = g_diagFifoDrawOpcode.load(std::memory_order_relaxed);
    if (outRawOk) *outRawOk = g_diagFifoRawOk.load(std::memory_order_relaxed);
    if (outRawFail) *outRawFail = g_diagFifoRawFail.load(std::memory_order_relaxed);
    if (outIncr) *outIncr = g_diagFifoIncrBegin.load(std::memory_order_relaxed);
    if (outNull) *outNull = g_diagFifoNullReset.load(std::memory_order_relaxed);
    if (outUnk) *outUnk = g_diagFifoUnknownByte.load(std::memory_order_relaxed);
    if (outBp) *outBp = g_diagFifoBpPkts.load(std::memory_order_relaxed);
    if (outCp) *outCp = g_diagFifoCpPkts.load(std::memory_order_relaxed);
    if (outXf) *outXf = g_diagFifoXfPkts.load(std::memory_order_relaxed);
    if (outInBegin) *outInBegin = g_hleGxState.inBegin ? 1u : 0u;
    if (outVertsRem) *outVertsRem = g_hleGxState.vertsRemaining;
    if (outBacklog) *outBacklog = static_cast<uint64_t>(g_hleGxState.fifoByteCount);
    if (outNAttr) {
        uint64_t n = 0;
        for (int i = 0; i < 26; ++i) if (g_hleGxState.vtxDesc[i] != GX_NONE) ++n;
        *outNAttr = n;
    }
}

extern "C" void GX_HLE_DiagFifoStallSnapshot(uint64_t* outNop, uint64_t* outBp, uint64_t* outCp,
                                             uint64_t* outXf, uint64_t* outIndx,
                                             uint64_t* outCallDl, uint64_t* outDraw,
                                             uint64_t* outAttr, uint64_t* outBpReg,
                                             uint64_t* outBpBacklog) {
    if (outNop) *outNop = g_diagFifoStallNop.load(std::memory_order_relaxed);
    if (outBp) *outBp = g_diagFifoStallBp.load(std::memory_order_relaxed);
    if (outCp) *outCp = g_diagFifoStallCp.load(std::memory_order_relaxed);
    if (outXf) *outXf = g_diagFifoStallXf.load(std::memory_order_relaxed);
    if (outIndx) *outIndx = g_diagFifoStallIndx.load(std::memory_order_relaxed);
    if (outCallDl) *outCallDl = g_diagFifoStallCallDl.load(std::memory_order_relaxed);
    if (outDraw) *outDraw = g_diagFifoStallDraw.load(std::memory_order_relaxed);
    if (outAttr) *outAttr = g_diagFifoStallAttr.load(std::memory_order_relaxed);
    if (outBpReg) *outBpReg = g_diagFifoStallBpReg.load(std::memory_order_relaxed);
    if (outBpBacklog) *outBpBacklog = g_diagFifoStallBpBacklog.load(std::memory_order_relaxed);
}

GXColor DecodeGxColor(uint32_t colorWord) {
    GXColor color{};
    color.r = static_cast<uint8_t>((colorWord >> 24) & 0xFF);
    color.g = static_cast<uint8_t>((colorWord >> 16) & 0xFF);
    color.b = static_cast<uint8_t>((colorWord >> 8) & 0xFF);
    color.a = static_cast<uint8_t>(colorWord & 0xFF);
    return color;
}

void WriteGuestFloat(uint32_t addr, float value, const char* label) {
    if (addr == 0) return;
    try { Memory::WriteFloat32(addr, value); } catch (const Memory::AccessViolation& e) { LogMemoryError(RT_TAG_GX, label ? label : "GX write", e); }
}

void* GuestToHostPtr(uint32_t addr, size_t len) {
    if (addr == 0) return nullptr;
    try { return Memory::GetPointer(addr, len); } catch (const Memory::AccessViolation& e) { LogMemoryError(RT_TAG_GX, "GX guest pointer", e); return nullptr; }
}

void WriteGuest32(uint32_t addr, uint32_t value, const char* label) {
    if (addr == 0) return;
    try { Memory::Write32(addr, value); } catch (const Memory::AccessViolation& e) { LogMemoryError(RT_TAG_GX, label ? label : "GX write32", e); }
}

bool IsKnownTexFormat(uint32_t fmt) {
    switch (fmt) {
    case GX_TF_I4: case GX_TF_I8: case GX_TF_IA4: case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_RGBA8: case GX_TF_CMPR: case GX_TF_C4: case GX_TF_C8: case GX_TF_C14X2: case GX_TF_Z8: case GX_TF_Z16: case GX_TF_Z24X8: case GX_CTF_R4: case GX_CTF_RA4: case GX_CTF_RA8: case GX_CTF_YUVA8: case GX_CTF_A8: case GX_CTF_R8: case GX_CTF_G8: case GX_CTF_B8: case GX_CTF_RG8: case GX_CTF_GB8: case GX_CTF_Z4: case GX_CTF_Z8M: case GX_CTF_Z8L: case GX_CTF_Z16L: return true;
    default: return false;
    }
}

bool IsPaletteTexFormat(uint32_t fmt) { return fmt == GX_TF_C4 || fmt == GX_TF_C8 || fmt == GX_TF_C14X2; }
bool IsKnownTlutFormat(uint32_t fmt) { switch (fmt) { case GX_TL_IA8: case GX_TL_RGB565: case GX_TL_RGB5A3: return true; default: return false; } }

// Returns false when the guest's TLUT descriptor is unusable. Callers must not
// hand the descriptor to aurora in that case: aurora reads entries*2 bytes from
// the pointer at load time, so a bad address or entry count is an out-of-range
// native read. This used to only log and let the call through, which is why the
// return value now exists.
bool ValidateTlutData(uint32_t objAddr, const TlutObjMeta& meta) {
    if (meta.dataAddr == 0 || meta.entries == 0) { RT_LOGF(RT_TAG_GX, "invalid TLUT data (oa=0x%08X)\n", objAddr); return false; }
    if (!IsKnownTlutFormat(meta.format)) { RT_LOGF(RT_TAG_GX, "invalid TLUT format 0x%X (oa=0x%08X)\n", meta.format, objAddr); return false; }
    if (!Memory::Contains(meta.dataAddr, static_cast<uint32_t>(meta.entries) * 2u)) {
        RT_LOGF(RT_TAG_GX, "invalid TLUT range (oa=0x%08X data=0x%08X entries=%u)\n", objAddr, meta.dataAddr, meta.entries);
        return false;
    }
    return true;
}
