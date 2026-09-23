// Boot/init hooks, OSFatal, and assorted hardware-init stubs.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "abi_bridge.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"
#include "system_bridge.h"

extern "C" void func_801A961C(CpuContext* ctx);
extern "C" void func_8055531C(CpuContext* ctx);

extern "C" void OSInitAlarm_RecompModLateInit_801a961c(CpuContext* ctx) {
    func_801A961C(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x801A961C, OSInitAlarm_RecompModLateInit_801a961c, "OSInitAlarm_RecompModLateInit_801a961c");

extern "C" void StaticRProlog_RecompModInit_8055531c(CpuContext* ctx) {
    RecompMod::RunMemoryInitializers();
    func_8055531C(ctx);
    RecompMod::RunPostRelInitializers();
}

REGISTER_NATIVE_FUNCTION_AS(0x8055531C, StaticRProlog_RecompModInit_8055531c, "StaticRProlog_RecompModInit_8055531c");

// ---------------------------------------------------------------------------
// USA (RMCE01) REL link path.
//
// CallModule invokes OSModuleHeader.prolog/epilog/unresolved. On USA these are
// 0x8055367C/0x805536C8/0x80553710 and are not in the translated graph (the
// PAL MAP/recomp scaffold uses 0x8055531C..). Natives below mirror the PAL
// static-R prolog pattern: RecompMod memory inits, null-terminated .ctors walk
// (skip unregistered), post-link work, RecompMod post-REL inits.
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kUsaCtorTable = 0x8088F350u;
constexpr uint32_t kUsaDtorTable = 0x8088F654u;
constexpr uint32_t kUsaRequestRootScene = 0x80542954u;
constexpr uint32_t kUsaSystemInstance = 0x802A4080u;
constexpr uint32_t kUsaVtableRootScene = 0x808B37E0u;
// USA SDA1 slot consumed by the post-ctor scene handoff (PAL uses 0x80385FC0).
constexpr uint32_t kUsaSceneHandoffSlot = 0x80381C48u;

bool TryReadGuest32(uint32_t address, uint32_t& out) {
    try {
        out = Memory::Read32(address);
        return true;
    } catch (const ::Memory::AccessViolation&) {
        return false;
    }
}

void WalkNullTerminatedGuestTable(CpuContext* cpu, uint32_t table, const char* what) {
    if (!table) {
        return;
    }
    int invoked = 0;
    int skipped = 0;
    for (uint32_t slot = table;; slot += 4) {
        uint32_t fn = 0;
        if (!TryReadGuest32(slot, fn)) {
            RT_LOG(RT_TAG_OS) << "USA REL " << what << ": unreadable slot 0x" << std::hex
                      << slot << std::dec << std::endl;
            break;
        }
        if (fn == 0) {
            break;
        }
        if (fn == 0xFFFFFFFFu) {
            continue;
        }
        if (!TranslatedFunctionRegistry::FindByAddressPtr(fn)) {
            ++skipped;
            continue;
        }
        InvokeIndirectCpu(fn, cpu);
        ++invoked;
    }
    RT_LOG(RT_TAG_OS) << "USA REL " << what << ": invoked=" << invoked
              << " skipped_unregistered=" << skipped << std::endl;
}

} // namespace

// RootScene request + SDA handoff that USA RelProlog tails into (0x80542954).
// Mid-function callees in the raw image (0x80008E44 / 0x80229A5C / 0x80009804)
// are unusable as call targets; use the registered bases PAL already exercises.
extern "C" void USARequestRootScene_80542954(CpuContext* ctx) {
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) {
        return;
    }

    cpu->gpr[3] = kUsaSystemInstance;
    if (TranslatedFunctionRegistry::FindByAddressPtr(0x80008E84u)) {
        InvokeDirectCpu<0x80008E84u>(cpu);
    } else {
        cpu->gpr[3] = kUsaSystemInstance;
    }
    const uint32_t system = cpu->gpr[3];
    if (system == 0) {
        RT_LOG(RT_TAG_OS) << "USA RequestRootScene: system instance is null" << std::endl;
        return;
    }

    uint32_t vtable = 0;
    uint32_t slot12 = 0;
    if (!TryReadGuest32(system, vtable) || !TryReadGuest32(vtable + 12, slot12) ||
        slot12 == 0 || !TranslatedFunctionRegistry::FindByAddressPtr(slot12)) {
        RT_LOG(RT_TAG_OS) << "USA RequestRootScene: skip virtual call, vt=0x" << std::hex
                  << vtable << " slot12=0x" << slot12 << std::dec << std::endl;
        return;
    }

    cpu->gpr[3] = system;
    InvokeIndirectCpu(slot12, cpu);

    // operator new(4, 4) via registered EGG heap entry (PAL: 0x80229DE0).
    const uint32_t virtualRet = cpu->gpr[3];
    cpu->gpr[4] = virtualRet;
    cpu->gpr[3] = 4;
    cpu->gpr[5] = 4;
    cpu->lr = 0x80542984u;
    if (TranslatedFunctionRegistry::FindByAddressPtr(0x80229DE0u)) {
        InvokeDirectCpu<0x80229DE0u>(cpu);
    } else {
        RT_LOG(RT_TAG_OS) << "USA RequestRootScene: operator new missing" << std::endl;
        return;
    }

    const uint32_t alloc = cpu->gpr[3];
    if (alloc != 0) {
        Memory::TryWrite32(alloc, kUsaVtableRootScene);
    } else {
        RT_LOG(RT_TAG_OS) << "USA RequestRootScene: alloc returned null" << std::endl;
    }

    uint32_t handoff = 0;
    if (!TryReadGuest32(kUsaSceneHandoffSlot, handoff) || handoff == 0) {
        RT_LOG(RT_TAG_OS) << "USA RequestRootScene: scene handoff slot empty (0x"
                  << std::hex << kUsaSceneHandoffSlot << "->" << handoff << std::dec
                  << "), skip ChangeScene" << std::endl;
        return;
    }
    uint32_t target = 0;
    if (!TryReadGuest32(handoff + 84, target) || target == 0) {
        RT_LOG(RT_TAG_OS) << "USA RequestRootScene: handoff+84 empty (base 0x"
                  << std::hex << handoff << std::dec << ")" << std::endl;
        return;
    }

    // PAL sets r5 = alloc before this tail; USA's raw bl does not re-materialize
    // it after new(), so pin it here (0x80009844 stores r5 at target+44).
    cpu->gpr[3] = target;
    cpu->gpr[4] = 0;
    cpu->gpr[5] = alloc;
    cpu->lr = 0x805429B0u;
    if (TranslatedFunctionRegistry::FindByAddressPtr(0x80009844u)) {
        InvokeDirectCpu<0x80009844u>(cpu);
    } else if (TranslatedFunctionRegistry::FindByAddressPtr(target)) {
        InvokeIndirectCpu(target, cpu);
    } else {
        RT_LOG(RT_TAG_OS) << "USA RequestRootScene: no change-scene target 0x"
                  << std::hex << target << std::dec << std::endl;
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x80542954, USARequestRootScene_80542954,
                            "USARequestRootScene_80542954");

extern "C" void USAStaticRProlog_RecompModInit_8055367c(CpuContext* ctx) {
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    RecompMod::RunMemoryInitializers();
    if (cpu) {
        const uint32_t savedR31 = cpu->gpr[31];
        WalkNullTerminatedGuestTable(cpu, kUsaCtorTable, "ctors");
        if (TranslatedFunctionRegistry::FindByAddressPtr(kUsaRequestRootScene)) {
            InvokeIndirectCpu(kUsaRequestRootScene, cpu);
        } else {
            RT_LOG(RT_TAG_OS) << "USA RelProlog: RequestRootScene 0x" << std::hex
                      << kUsaRequestRootScene << std::dec << " not registered" << std::endl;
        }
        cpu->gpr[31] = savedR31;
    }
    RecompMod::RunPostRelInitializers();
}

REGISTER_NATIVE_FUNCTION_AS(0x8055367C, USAStaticRProlog_RecompModInit_8055367c,
                            "USAStaticRProlog_RecompModInit_8055367c");

extern "C" void USAStaticREpilog_RecompMod_805536c8(CpuContext* ctx) {
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) {
        return;
    }
    const uint32_t savedR31 = cpu->gpr[31];
    WalkNullTerminatedGuestTable(cpu, kUsaDtorTable, "dtors");
    cpu->gpr[31] = savedR31;
}

REGISTER_NATIVE_FUNCTION_AS(0x805536C8, USAStaticREpilog_RecompMod_805536c8,
                            "USAStaticREpilog_RecompMod_805536c8");

// Unresolved-import stub: report and continue instead of OSFatal so a missing
// HLE does not take down the boot once prolog/epilog are past.
extern "C" void USAStaticRUnresolved_80553710(CpuContext* ctx) {
    RT_LOG(RT_TAG_OS) << "USA REL unresolved handler called (r3=0x" << std::hex
              << (ctx ? static_cast<uint32_t>(ctx->gpr[3]) : 0u) << std::dec
              << "); continuing" << std::endl;
}

REGISTER_NATIVE_FUNCTION_AS(0x80553710, USAStaticRUnresolved_80553710,
                            "USAStaticRUnresolved_80553710");

namespace {
std::string ReadGuestCStringLimited(uint32_t address, size_t limit = 4096) {
    if (address == 0) {
        return "<null>";
    }

    std::string text;
    text.reserve(128);
    for (size_t i = 0; i < limit; ++i) {
        const uint8_t ch = Memory::Read8(address + static_cast<uint32_t>(i));
        if (ch == 0) {
            return text;
        }
        text.push_back(static_cast<char>(ch));
    }
    text += "<unterminated>";
    return text;
}
}

extern "C" void OSFatal_HLE_801a4ec4(CpuContext* ctx) {
    const uint32_t fg = ctx ? ctx->gpr[3] : 0;
    const uint32_t bg = ctx ? ctx->gpr[4] : 0;
    const uint32_t messagePtr = ctx ? ctx->gpr[5] : 0;
    RT_LOG(RT_TAG_OS) << "OS::Fatal called fg=0x" << std::hex << fg
              << " bg=0x" << bg
              << " message=0x" << messagePtr
              << std::dec << " '" << ReadGuestCStringLimited(messagePtr) << "'" << std::endl;
    if (ctx) {
        SystemBridge::DumpCpuState(ctx);
    }
    const std::string guestMessage = ReadGuestCStringLimited(messagePtr);
    const std::string details =
        guestMessage.empty() ? std::string("OS::Fatal was called without a message.") : guestMessage;
    // MarkFatalErrorReported below suppresses the atexit reporter, so this path
    // has to write its own artifacts or the run folder gets nothing.
    RuntimeCrash::WriteCrashArtifacts("osfatal", details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system reported a fatal error", details);
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

REGISTER_NATIVE_FUNCTION_AS(0x801A4EC4, OSFatal_HLE_801a4ec4, "OSFatal_HLE_801a4ec4");

extern "C" void GKI_delay_HLE_801301b4(CpuContext* ctx)
{
    const uint32_t delayMs = ctx ? static_cast<uint32_t>(ctx->gpr[3]) : 0;
    const uint32_t sleepMs = delayMs == 0 ? 1u : std::min(delayMs, 10u);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
}

extern "C" uint32_t BTM_IsDeviceUp_HLE_8013a300(CpuContext* ctx)
{
    // Force Bluetooth stack to "up" to avoid endless polling loops while we lack
    // real hardware bring-up.
    constexpr uint32_t kBtmCbBase = 0x80336278u;
    constexpr uint32_t kDevStateOffset = 0x64Eu;
    try {
        ::Memory::Write8(kBtmCbBase + kDevStateOffset, 5u);
    } catch (const ::Memory::AccessViolation&) {
        // Ignore; best-effort write
    }

    if (ctx) {
        ctx->gpr[3] = 1;
    }
    return 1;
}

PPC_NATIVE_OVERRIDE_VOID(801301B4, GKI_delay_HLE_801301b4, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE(8013A300, BTM_IsDeviceUp_HLE_8013a300, uint32_t, (CpuContext* ctx), (ctx));

// Serial Interface (SI) - GameCube controller ports; stubbed since we don't emulate the MMIO.

// SIInit (0x801b2de0): skips MMIO setup at 0xCD006434 and controller detection.
extern "C" void SIInit_801b2de0()
{
    RT_LOG(RT_TAG_OS) << "SIInit_801b2de0 called: skipping MMIO register setup and controller detection" << std::endl;
}

// SISetSamplingRate (0x801b3acc): ignored, we don't emulate SI polling timing.
extern "C" void HLE_SISetSamplingRate_801b3acc(uint32_t msec)
{
    RT_LOG(RT_TAG_OS) << "HLE_SISetSamplingRate_801b3acc called: msec=" << msec << ": Stubbed success." << std::endl;
}

// Video Interface (VI) - TV output.

// VIGetTvFormat (0x801bacd8): CRITICAL, must match the disc region —
// 1 (VI_PAL) for PAL builds, 0 (VI_NTSC) for USA/JPN — or the game
// misbehaves/panics (PAL-on-NTSC drops to the half-rate PAL50 sync path,
// NTSC-on-PAL fails its video-mode checks the same way).
extern "C" uint32_t HLE_VIGetTvFormat_801bacd8()
{
    // VI_NTSC = 0, VI_PAL = 1, VI_MPAL = 2
    // Region byte is the 4th char of the game code at 0x80000000
    // (RMCP/RMCJ = PAL, RMCE = NTSC-U, RMCJ handled as NTSC).
    uint32_t format = 1;
    if (Memory::Contains(0x80000000u, 4u)) {
        const char region =
            static_cast<char>((Memory::Read32(0x80000000u) >> 8) & 0xFFu);
        if (region == 'E' || region == 'J') {
            format = 0;
        }
    }
    RT_LOG(RT_TAG_OS) << "HLE_VIGetTvFormat_801bacd8 called: returning " << format
                      << (format == 1 ? " (VI_PAL)" : " (VI_NTSC)") << std::endl;
    return format;
}

REGISTER_NATIVE_FUNCTION(0x801B2DE0, SIInit_801b2de0);
REGISTER_NATIVE_FUNCTION(0x801B2D40, SIInit_801b2de0); // USA
PPC_NATIVE_OVERRIDE_VOID(801B3ACC, HLE_SISetSamplingRate_801b3acc, (uint32_t msec), (msec));

// OS____InitMemoryProtection (0x801A7DFC): real version touches MMU/MMIO we don't emulate;
// no-op and return success so boot doesn't stall.
extern "C" uint32_t OS____InitMemoryProtection_801a7dfc(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8)
{
    RT_LOG(RT_TAG_OS) << "OS____InitMemoryProtection_801a7dfc called (stubbed): r3=0x" << std::hex << r3 << std::dec << std::endl;

    return 0;
}

PPC_NATIVE_OVERRIDE(801A7DFC, OS____InitMemoryProtection_801a7dfc, uint32_t, (uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8), (r3, r4, r5, r6, r7, r8));

// OSGetConsoleType (0x8019f33c): standard Wii = 0x12, NDEV (expanded MEM2) = 0x10000012;
// MKWii uses the NDEV result to enable its extra-memory heap path.
extern "C" uint32_t OS__GetConsoleType_8019f33c(uint32_t /*r4*/, uint32_t /*r5*/, uint32_t /*r6*/,
                                                uint32_t /*r7*/, uint32_t /*r8*/, uint32_t /*r31*/)
{
    constexpr uint32_t kRetailMem2Size = 64u * 1024u * 1024u;
    const uint32_t physicalMem2Size = Memory::Read32(0x80003118u);
    const uint32_t consoleType =
        physicalMem2Size == kRetailMem2Size ? 0x00000012u : 0x10000012u;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        RT_LOG(RT_TAG_OS) << "OSGetConsoleType: MEM2="
                  << (physicalMem2Size / (1024u * 1024u))
                  << " MB, returning 0x" << std::hex << consoleType << std::dec << std::endl;
    }
    return consoleType;
}

// Register the function
PPC_NATIVE_OVERRIDE(8019F33C, OS__GetConsoleType_8019f33c, uint32_t, (uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r31), (r4, r5, r6, r7, r8, r31));

// OSGetResetCode (0x801a8a50): real version reads MMIO 0xCC003024; we always report
// Cold Boot (0).
extern "C" uint32_t OSGetResetCode_801a8a50()
{
    // Log occasionally just to track boot flow
    static bool logged = false;
    if (!logged) {
        RT_LOG(RT_TAG_OS) << "OSGetResetCode_801a8a50 called: returning 0 (Cold Boot)" << std::endl;
        logged = true;
    }
    return 0; 
}

// Register the function
PPC_NATIVE_OVERRIDE(801A8A50, OSGetResetCode_801a8a50, uint32_t, (), ());

// __OSInitSTM (0x801AB848): real version opens /dev/stm/* handles. We stub it by writing
// fake handles and the success flag into the SDA (r13) block so OSResetSystem's checks pass.
extern "C" uint32_t __OSInitSTM_HLE_801ab848(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) return 0;

    RT_LOG(RT_TAG_OS) << "__OSInitSTM_HLE_801ab848 called: initializing STM state" << std::endl;

    // R13 (SDA2) holds the base for small data variables
    const uint32_t r13 = cpu->gpr[13];
    if (r13 == 0) {
         RT_LOG(RT_TAG_OS) << "__OSInitSTM: Warning - R13 is 0, cannot write state." << std::endl;
         return 0;
    }

    // Offsets from disassembly: r13-0x62cc=STM_Initialized, r13-0x62c8=/dev/stm/immediate,
    // r13-0x62c4=/dev/stm/eventhook.
    try {
        // Mark STM as initialized
        ::Memory::Write32(r13 - 0x62ccu, 1);

        // Fake non-zero handles so callers' zero-checks pass.
        ::Memory::Write32(r13 - 0x62c8u, 0x00535401); // "ST\x01"
        ::Memory::Write32(r13 - 0x62c4u, 0x00535402); // "ST\x02"

        // Default Power/Reset callback pointers are left unset; safe since we never fire
        // the STM hardware interrupt that would invoke them.
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "__OSInitSTM: Failed to write STM state to SDA @ 0x"
                  << std::hex << e.address() << std::dec << " (" << e.reason() << ")" << std::endl;
        return 0; // Return failure
    }

    // Return 1 (success)
    return 1;
}

// Register the function
PPC_NATIVE_OVERRIDE(801AB848, __OSInitSTM_HLE_801ab848, uint32_t, (CpuContext* ctx), (ctx));


extern "C" void OS____PSInit_801a04a0()
{
    RT_LOG(RT_TAG_OS) << "OS____PSInit_801a04a0 called (stubbed)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(801A04A0, OS____PSInit_801a04a0, (), ());



extern "C" void __init_hardware_80006348()
{
    RT_LOG(RT_TAG_OS) << "__init_hardware_80006348 called (stubbed)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(80006348,__init_hardware_80006348, (), ());


// PPC SPR (Special Purpose Register) access stubs; these registers don't exist on x86 so
// each one just logs and no-ops.

// Each PPC_NATIVE_OVERRIDE_VOID line stays written out per stub (not looped) because the
// translator text-scans them to decide which addresses to skip translating.
#define PPC_SPR_STUB_BODY(name, message) \
    extern "C" void name() { RT_LOG(RT_TAG_OS) << message << std::endl; }

PPC_SPR_STUB_BODY(PPCMfhid0_8012e574, "PPCMfhid0 called (stubbed) - Move From HID0")
PPC_NATIVE_OVERRIDE_VOID(8012e574, PPCMfhid0_8012e574, (), ());

PPC_SPR_STUB_BODY(PPCMthid0_8012e57c, "PPCMthid0 called (stubbed) - Move To HID0")
PPC_NATIVE_OVERRIDE_VOID(8012e57c, PPCMthid0_8012e57c, (), ());

extern "C" void PPCMtdec_8012e594()
{
    static std::atomic<int> logCount{0};
    if (logCount.fetch_add(1) < 4) {
        RT_LOG(RT_TAG_OS) << "PPCMtdec called (stubbed) - Move To Decrementer" << std::endl;
    }
    // We don't simulate decrementer exceptions, so pump due alarms here instead. Capped at
    // 32, not 1: a single alarm let unrelated periodic alarms backlog and delay
    // AsyncDisplay's pacing alarm by multiple retraces.
    OS_HLE_ProcessAlarms(32);
}
PPC_NATIVE_OVERRIDE_VOID(8012E594, PPCMtdec_8012e594, (), ());

extern "C" void PPCSync_8012e59c()
{
    // PowerPC `sync`. Translated guest code runs on one host thread at a time
    // and the runtime's own cross-thread state uses C++ atomics, so there is no
    // guest-visible reordering for this barrier to prevent.
}
PPC_NATIVE_OVERRIDE_VOID(8012e59c, PPCSync_8012e59c, (), ());

PPC_SPR_STUB_BODY(PPCMtmmcr0_8012e5b8, "PPCMtmmcr0 called (stubbed) - Move To MMCR0")
PPC_NATIVE_OVERRIDE_VOID(8012e5b8, PPCMtmmcr0_8012e5b8, (), ());

PPC_SPR_STUB_BODY(PPCMtmmcr1_8012e5c0, "PPCMtmmcr1 called (stubbed) - Move To MMCR1")
PPC_NATIVE_OVERRIDE_VOID(8012e5c0, PPCMtmmcr1_8012e5c0, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc1_8012e5c8, "PPCMtpmc1 called (stubbed) - Move To PMC1")
PPC_NATIVE_OVERRIDE_VOID(8012e5c8, PPCMtpmc1_8012e5c8, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc2_8012e5d0, "PPCMtpmc2 called (stubbed) - Move To PMC2")
PPC_NATIVE_OVERRIDE_VOID(8012e5d0, PPCMtpmc2_8012e5d0, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc3_8012e5d8, "PPCMtpmc3 called (stubbed) - Move To PMC3")
PPC_NATIVE_OVERRIDE_VOID(8012e5d8, PPCMtpmc3_8012e5d8, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc4_8012e5e0, "PPCMtpmc4 called (stubbed) - Move To PMC4")
PPC_NATIVE_OVERRIDE_VOID(8012e5e0, PPCMtpmc4_8012e5e0, (), ());

extern "C" uint32_t PPCMfhid2_8012e630_impl()
{
    if (CpuContext* cpu = TryGetCpuContext()) {
        if (cpu->hid2 == 0) {
            cpu->hid2 = 0x10000000u;
        }
        return cpu->hid2;
    }
    return 0x10000000u;
}
// Register the function using a return-value stub
extern "C" void PPCMfhid2_HLE_8012e630(CpuContext* ctx)
{
    ctx->gpr[3] = PPCMfhid2_8012e630_impl();
}
REGISTER_TRANSLATED_FUNCTION(0x8012e630, PPCMfhid2_HLE_8012e630);

extern "C" void PPCMthid2_8012e638(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    cpu->hid2 = cpu->gpr[3];

    static std::atomic<int> logCount{0};
    if (logCount.fetch_add(1) < 4) {
        RT_LOG(RT_TAG_OS) << "PPCMthid2 set HID2=0x" << std::hex << cpu->hid2 << std::dec << std::endl;
    }
}
PPC_NATIVE_OVERRIDE_VOID(8012e638, PPCMthid2_8012e638, (CpuContext* ctx), (ctx));

PPC_SPR_STUB_BODY(PPCMfwpar_8012e640,
                  "PPCMfwpar called (stubbed) - Move From Write Pipe Address Register")
PPC_NATIVE_OVERRIDE_VOID(8012e640, PPCMfwpar_8012e640, (), ());

PPC_SPR_STUB_BODY(PPCMtwpar_8012e64c,
                  "PPCMtwpar called (stubbed) - Move To Write Pipe Address Register")
PPC_NATIVE_OVERRIDE_VOID(8012e64c, PPCMtwpar_8012e64c, (), ());

PPC_SPR_STUB_BODY(PPCDisableSpeculation_8012e654, "PPCDisableSpeculation called (stubbed)")
PPC_NATIVE_OVERRIDE_VOID(8012e654, PPCDisableSpeculation_8012e654, (), ());

PPC_SPR_STUB_BODY(PPCMthid4_8012e684, "PPCMthid4 called (stubbed) - Move To HID4")
PPC_NATIVE_OVERRIDE_VOID(8012e684, PPCMthid4_8012e684, (), ());
