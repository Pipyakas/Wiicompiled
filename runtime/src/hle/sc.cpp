#include "hle_stubs.h"

#include "console_identity.h"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

namespace {

// Region byte is the 4th char of the game code at 0x80000000
// (RMCP = PAL, RMCE = NTSC-U, RMCJ = NTSC-J). SeedLowMemDefaults publishes
// the real disc header there before boot, so SC answers follow the disc
// instead of hardcoding PAL. Falls back to PAL when lowmem is not up yet.
char DiscRegionByte()
{
    if (Memory::Contains(0x80000000u, 4u)) {
        const uint32_t code = Memory::Read32(0x80000000u);
        bool valid = true;
        for (int shift = 24; shift >= 0; shift -= 8) {
            const char ch = static_cast<char>((code >> shift) & 0xFFu);
            if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z'))) {
                valid = false;
                break;
            }
        }
        if (valid) {
            const char region = static_cast<char>((code >> 8) & 0xFFu);
            if (region == 'E' || region == 'J' || region == 'P') {
                return region;
            }
        }
    }
    return 'P';
}

} // namespace

// SCCheckStatus is polled in OSInit's busy loop (while(SCCheckStatus()==1) waits on async SYSCONF
// load via NAND IPC); we have no async IPC callbacks, so return 0 (SUCCESS) immediately.

// 0x801B0220 -> SCCheckStatus()
// Returns: 0 = success, 1 = busy, 2 = error
extern "C" uint32_t SCCheckStatus_HLE()
{
    // Return 0 (success) immediately to avoid infinite busy-wait in OSInit
    return 0;
}

PPC_NATIVE_OVERRIDE(801B0220, SCCheckStatus_HLE, uint32_t, (), ());

// 0x801B1BE4 -> SCGetAspectRatio()
// Returns: 0 = 4:3, 1 = 16:9
extern "C" uint32_t SCGetAspectRatio_HLE()
{
    return RuntimeConfigFile::WidescreenEnabled(true) ? 1u : 0u;
}

PPC_NATIVE_OVERRIDE(801B1BE4, SCGetAspectRatio_HLE, uint32_t, (), ());

// 0x801B1CAC -> SCGetEuRgb60Mode()
// Returns: 0 = PAL50, 1 = PAL60/RGB60
extern "C" uint32_t SCGetEuRgb60Mode_HLE()
{
    // Default to PAL60 so PAL builds do not fall back to the half-rate PAL50
    // sync path on modern displays. Actual texture/cache correctness is handled
    // elsewhere; this only exposes the intended SYSCONF setting.
    return 1;
}

PPC_NATIVE_OVERRIDE(801B1CAC, SCGetEuRgb60Mode_HLE, uint32_t, (), ());

// The managed NAND intentionally starts without a console-owned setting.txt.
// DWC nevertheless requires the Wii product code and serial number so it can
// include csnum in NAS authentication. Expose one stable virtual-console
// identity without requiring or mutating a user's real NAND.

extern "C" uint32_t SCGetProductArea_HLE()
{
    // The SDK's lookup table at 0x8029CEB0 maps JPN=0, USA=1, EUR=2.
    // USA discs report USA; PAL/JPN fall back to the EUR/JPN entries.
    switch (DiscRegionByte()) {
    case 'E': return 1;
    case 'J': return 0;
    default: return 2;
    }
}

PPC_NATIVE_OVERRIDE(801B23A0, SCGetProductArea_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductCode_HLE()
{
    // Original SC storage for the product CODE value (PAL "LEH", USA "LU").
    constexpr uint32_t kProductCodeAddress = 0x803869E0u;
    static constexpr char kPalProductCode[] = "LEH";
    static constexpr char kUsaProductCode[] = "LU";
    const char* code = (DiscRegionByte() == 'E') ? kUsaProductCode : kPalProductCode;
    const size_t size = std::strlen(code) + 1;
    if (!Memory::Contains(kProductCodeAddress, size)) {
        return 0;
    }
    std::memcpy(Memory::GetPointer(kProductCodeAddress, size), code, size);
    return kProductCodeAddress;
}

PPC_NATIVE_OVERRIDE(801B2424, SCGetProductCode_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductSN_HLE(uint32_t serialAddress)
{
    const std::string& serial = RuntimeConsoleIdentity::Current().serial;
    if (!serialAddress || !Memory::Contains(serialAddress, serial.size() + 1)) {
        return 0;
    }
    std::memcpy(Memory::GetPointer(serialAddress, serial.size() + 1),
                serial.c_str(), serial.size() + 1);
    return 1;
}

PPC_NATIVE_OVERRIDE(801B2460, SCGetProductSN_HLE, uint32_t, (uint32_t serialAddress), (serialAddress));

extern "C" uint32_t SCGetProductGameRegion_HLE()
{
    // The SDK's own lookup table at 0x8029CEF8 maps JP=0, US=1, EU=2.
    switch (DiscRegionByte()) {
    case 'E': return 1;
    case 'J': return 0;
    default: return 2;
    }
}

PPC_NATIVE_OVERRIDE(801B24C8, SCGetProductGameRegion_HLE, uint32_t, (), ());

// These stubs make the game think all titles are installed; otherwise it checks title ID
// 0x00010004524d4350 ("RMCP", Mario Kart Wii PAL) and reports error code 5.

// 0x801AE4A0 -> OS__IsTitleInstalled(titleIdHi, titleIdLo)
// Returns: 1 = installed, 0 = not installed
extern "C" uint32_t OS__IsTitleInstalled(uint32_t titleIdHi, uint32_t titleIdLo)
{
    RT_LOGF(RT_TAG_HLE, "CINS: OSIsTitleInstalled(0x%08X%08X) -> 1 (stubbed as installed)\n",
            titleIdHi, titleIdLo);
    return 1; // Always report installed
}

PPC_NATIVE_OVERRIDE(801AE4A0, OS__IsTitleInstalled, uint32_t, (uint32_t titleIdHi, uint32_t titleIdLo), (titleIdHi, titleIdLo));

// 0x801AD1D4 -> OS__CheckInstall(requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr): returns 0 with
// outFlagsPtr = 0x3 (bit0 has data, bit1 has update; bit2 would be needs-blocks) i.e. fully installed.
extern "C" uint32_t OS__CheckInstall(uint32_t requiredBlocks, uint32_t titleIdHi, 
                                      uint32_t titleIdLo, uint32_t outFlagsPtr)
{
    RT_LOGF(RT_TAG_HLE, "OS__CheckInstall(blocks=%u, 0x%08X%08X) -> success (stubbed)\n",
            requiredBlocks, titleIdHi, titleIdLo);
    if (outFlagsPtr != 0) {
        Memory::Write32(outFlagsPtr, 0x3); // has data + has update = fully installed
    }
    return 0; // Success
}

PPC_NATIVE_OVERRIDE(801AD1D4, OS__CheckInstall, uint32_t, (uint32_t requiredBlocks, uint32_t titleIdHi, uint32_t titleIdLo, uint32_t outFlagsPtr), (requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr));
