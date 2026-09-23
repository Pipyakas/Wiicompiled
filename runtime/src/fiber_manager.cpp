#include "fiber_manager.h"
#include "memory.h"
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "runtime_log.h"
#ifndef MKW_RUNTIME_CONFIG_HEADER
#define MKW_RUNTIME_CONFIG_HEADER "generated/RuntimeConfig.h"
#endif
#include MKW_RUNTIME_CONFIG_HEADER

// Defined in hle/os/os_sleep.cpp; the sleep-timer table is file-local there.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <sstream>

#if !defined(_WIN32)
#include "libco.h"
#endif

namespace Fiber {

#if !defined(_WIN32)
namespace {
// libco's co_create() entry points take no argument, unlike CreateFiber(size, FiberProc, param).
// CreateGuestFiber() stages the guest thread address here immediately before the first co_switch
// into a freshly created cothread; FiberProcTrampoline reads it exactly once, at the top of the
// fiber's very first activation. Safe because guest fibers are strictly cooperative on a single
// OS thread: nothing else can run (and so nothing else can overwrite this) between the staging
// write and the trampoline's read of it.
thread_local uint32_t s_pendingFiberArg = 0;
} // namespace
#endif

std::mutex GuestFiberManager::s_mutex;
std::unordered_map<uint32_t, GuestFiber> GuestFiberManager::s_fibers;
std::vector<void*> GuestFiberManager::s_fibersPendingDelete;
void* GuestFiberManager::s_schedulerFiber = nullptr;
uint32_t GuestFiberManager::s_currentGuestThread = 0;
bool GuestFiberManager::s_initialized = false;
thread_local CpuContext* GuestFiberManager::s_cpuContext = nullptr;

void GuestFiberManager::PurgePendingFibers() {
    std::vector<void*> toDelete;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        toDelete.swap(s_fibersPendingDelete);
    }
#if defined(_WIN32)
    const void* current = GetCurrentFiber();
    for (void* f : toDelete) {
        if (f && f != current) {
            DeleteFiber(f);
        }
    }
#else
    const void* current = co_active();
    for (void* f : toDelete) {
        if (f && f != current) {
            co_delete(static_cast<cothread_t>(f));
        }
    }
#endif
}

// Global VI retrace counter
std::atomic<uint32_t> g_viRetracePendingCount{0};

// =============================================================================
// Guest OS memory layout constants
// =============================================================================
namespace {
constexpr uint32_t kOSCurrentContextAddr = 0x800000d4u;
constexpr uint32_t kOSRunningContextAddr = 0x800000e4u;  // OSGetCurrentThread reads this
constexpr uint32_t kThreadQueueArrayAddr = 0x803477b0u;
constexpr uint32_t kSchedulerPendingFlagAddr = 0x80386920u;
constexpr uint32_t kSchedulerReschedCounterAddr = 0x8038691cu;
constexpr uint32_t kSchedulerIdleFlagAddr = 0x80386918u;

// OSThread structure offsets
constexpr uint32_t kThreadStateOffset = 0x2C8u;
constexpr uint32_t kThreadAttrOffset = 0x2CAu;
constexpr uint32_t kThreadSuspendOffset = 0x2CCu;
constexpr uint32_t kThreadPriorityOffset = 0x2D0u;
constexpr uint32_t kThreadExitValueOffset = 0x2D8u;
constexpr uint32_t kThreadNextOffset = 0x2E0u;
constexpr uint32_t kThreadPrevOffset = 0x2E4u;
constexpr uint32_t kThreadQueueOffset = 0x2DCu;
constexpr uint32_t kThreadJoinQueueOffset = 0x2E8u;

// OSContext offsets (for saving/loading fiber context)
constexpr uint32_t kCtxGprOffset = 0x00u;
constexpr uint32_t kCtxCrOffset = 0x80u;
constexpr uint32_t kCtxLrOffset = 0x84u;
constexpr uint32_t kCtxCtrOffset = 0x88u;
constexpr uint32_t kCtxXerOffset = 0x8Cu;
constexpr uint32_t kCtxSrr0Offset = 0x198u;
constexpr uint32_t kCtxSrr1Offset = 0x19Cu;
constexpr uint32_t kCtxGqrOffset = 0x1A8u;

void ClearPendingMaskForEmptyGuestQueue(uint32_t queueEntry)
{
    if (queueEntry < kThreadQueueArrayAddr ||
        ((queueEntry - kThreadQueueArrayAddr) % 8u) != 0) {
        return;
    }

    const uint32_t priority = (queueEntry - kThreadQueueArrayAddr) / 8u;
    if (priority >= 32 || Memory::Read32(queueEntry) != 0) {
        return;
    }

    const uint32_t pending = Memory::Read32(kSchedulerPendingFlagAddr);
    Memory::Write32(kSchedulerPendingFlagAddr, pending & ~(1u << (31u - priority)));
}

void RemoveGuestThreadFromQueue(uint32_t threadPtr)
{
    const uint32_t queuePtr = Memory::Read32(threadPtr + kThreadQueueOffset);
    if (queuePtr == 0) {
        return;
    }

    const uint32_t next = Memory::Read32(threadPtr + kThreadNextOffset);
    const uint32_t prev = Memory::Read32(threadPtr + kThreadPrevOffset);

    if (next != 0) {
        Memory::Write32(next + kThreadPrevOffset, prev);
    } else {
        Memory::Write32(queuePtr + 4u, prev);
    }

    if (prev != 0) {
        Memory::Write32(prev + kThreadNextOffset, next);
    } else {
        Memory::Write32(queuePtr, next);
    }

    Memory::Write32(threadPtr + kThreadQueueOffset, 0);
    Memory::Write32(threadPtr + kThreadNextOffset, 0);
    Memory::Write32(threadPtr + kThreadPrevOffset, 0);
    ClearPendingMaskForEmptyGuestQueue(queuePtr);
}

void WakeGuestThreadsOnQueueNoSwitch(uint32_t queueAddr)
{
    constexpr int kMaxWake = 256;

    for (int woke = 0; woke < kMaxWake; ++woke) {
        const uint32_t thread = Memory::Read32(queueAddr);
        if (thread == 0) {
            return;
        }

        const uint32_t next = Memory::Read32(thread + kThreadNextOffset);
        if (next == 0) {
            Memory::Write32(queueAddr + 4u, 0);
        } else {
            Memory::Write32(next + kThreadPrevOffset, 0);
        }
        Memory::Write32(queueAddr, next);
        Memory::Write32(thread + kThreadNextOffset, 0);
        Memory::Write32(thread + kThreadPrevOffset, 0);

        const uint16_t state = Memory::Read16(thread + kThreadStateOffset);
        if (state == 0 || state == 8) {
            Memory::Write32(thread + kThreadQueueOffset, 0);
            continue;
        }

        Memory::Write16(thread + kThreadStateOffset, 1);
        const int32_t suspend = static_cast<int32_t>(Memory::Read32(thread + kThreadSuspendOffset));
        if (suspend >= 1) {
            Memory::Write32(thread + kThreadQueueOffset, 0);
            continue;
        }

        int32_t priority = static_cast<int32_t>(Memory::Read32(thread + kThreadPriorityOffset));
        priority = std::clamp(priority, 0, 31);

        const uint32_t runQueue = kThreadQueueArrayAddr + static_cast<uint32_t>(priority) * 8u;
        const uint32_t tail = Memory::Read32(runQueue + 4u);
        if (tail == 0) {
            Memory::Write32(runQueue, thread);
        } else {
            Memory::Write32(tail + kThreadNextOffset, thread);
        }
        Memory::Write32(thread + kThreadPrevOffset, tail);
        Memory::Write32(thread + kThreadNextOffset, 0);
        Memory::Write32(runQueue + 4u, thread);
        Memory::Write32(thread + kThreadQueueOffset, runQueue);

        const uint32_t pending = Memory::Read32(kSchedulerPendingFlagAddr);
        Memory::Write32(kSchedulerPendingFlagAddr, pending | (1u << (31u - static_cast<uint32_t>(priority))));
        Memory::Write32(kSchedulerReschedCounterAddr, 1);

        GuestFiberManager::ResumeGuestThread(thread);
    }

    RT_LOG(RT_TAG_OS) << "WakeGuestThreadsOnQueueNoSwitch: hit safety limit at 0x"
              << std::hex << queueAddr << std::dec << std::endl;
}
} // namespace

// =============================================================================
// GuestFiberManager Implementation
// =============================================================================

void GuestFiberManager::Initialize() {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    if (s_initialized) {
        return;
    }
    
#if defined(_WIN32)
    // Convert the main thread to a fiber (the scheduler fiber)
    s_schedulerFiber = ConvertThreadToFiber(nullptr);
    if (!s_schedulerFiber) {
        // May already be a fiber
        s_schedulerFiber = GetCurrentFiber();
        if (!s_schedulerFiber) {
            RT_LOG(RT_TAG_OS) << "FATAL: Failed to initialize scheduler fiber!" << std::endl;
            ShowRuntimeFatalPopup("guest scheduler initialization failed",
                                  "Windows could not create the scheduler fiber required to run guest threads.");
            std::abort();
        }
    }
    
#else
    // co_active() returns a handle for whichever native stack is currently running, creating one
    // on first call if needed - the libco analogue of ConvertThreadToFiber(nullptr): it converts
    // this call's own stack into a switchable target without altering control flow.
    s_schedulerFiber = co_active();
#endif

    s_currentGuestThread = 0;
    s_initialized = true;
}

void GuestFiberManager::Shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);

#if defined(_WIN32)
    for (auto& [addr, fiber] : s_fibers) {
        if (fiber.fiber && !fiber.isSchedulerFiber) {
            DeleteFiber(fiber.fiber);
            fiber.fiber = nullptr;
        }
    }
    s_fibers.clear();

    // Convert scheduler fiber back to thread
    if (s_schedulerFiber) {
        ConvertFiberToThread();
        s_schedulerFiber = nullptr;
    }
#else
    for (auto& [addr, fiber] : s_fibers) {
        if (fiber.fiber && !fiber.isSchedulerFiber) {
            co_delete(static_cast<cothread_t>(fiber.fiber));
            fiber.fiber = nullptr;
        }
    }
    s_fibers.clear();
    // Unlike ConvertFiberToThread, libco has no "undo" for co_active(): the scheduler's own
    // stack was never separately allocated, so there is nothing to release here.
    s_schedulerFiber = nullptr;
#endif

    s_initialized = false;
}

bool GuestFiberManager::IsInitialized() {
    return s_initialized;
}

bool GuestFiberManager::CreateGuestFiber(uint32_t guestThreadAddr, uint32_t entryPoint,
                                          uint32_t entryArg, uint32_t stackBase) {
    if (!s_initialized) {
        RT_LOG(RT_TAG_OS) << "CreateGuestFiber called before initialization!" << std::endl;
        return false;
    }
    
    std::lock_guard<std::mutex> lock(s_mutex);
    
    // Check if fiber already exists for this thread - if so, reset it
    auto existingIt = s_fibers.find(guestThreadAddr);
    if (existingIt != s_fibers.end()) {
        // Delete the old fiber if it exists and is not the scheduler fiber
        if (existingIt->second.fiber && !existingIt->second.isSchedulerFiber) {
#if defined(_WIN32)
            DeleteFiber(existingIt->second.fiber);
#else
            co_delete(static_cast<cothread_t>(existingIt->second.fiber));
#endif
        }
        s_fibers.erase(existingIt);
    }
    
    GuestFiber gf;
    gf.entryPoint = entryPoint;
    gf.entryArg = entryArg;
    gf.state = ThreadState::WAITING; // Starts suspended
    gf.terminated = false;
    gf.isSchedulerFiber = false;
    
    // Initialize CPU context with entry point info
    std::memset(&gf.cpuContext, 0, sizeof(CpuContext));
    gf.cpuContext.gpr[1] = stackBase; // Stack pointer
    gf.cpuContext.gpr[3] = entryArg;  // First argument
    gf.cpuContext.lr = 0;             // No return address
    gf.cpuContext.pc = entryPoint;
    gf.cpuContext.srr0 = entryPoint;
    
#if defined(_WIN32)
    // Create Windows fiber with reasonable stack size
    // Use host stack size (64KB should be plenty for translated code)
    constexpr size_t kHostStackSize = 64 * 1024;
    gf.fiber = CreateFiber(kHostStackSize, FiberProc, reinterpret_cast<void*>(static_cast<uintptr_t>(guestThreadAddr)));
    
    if (!gf.fiber) {
        DWORD err = GetLastError();
        RT_LOG(RT_TAG_OS) << "CreateFiber failed for thread 0x"
                  << std::hex << guestThreadAddr 
                  << " error=" << std::dec << err << std::endl;
        return false;
    }
#else
    // libco's co_create() entry point takes no argument; SwitchToThread() stages guestThreadAddr
    // into s_pendingFiberArg immediately before the co_switch that first activates this handle.
    constexpr unsigned int kHostStackSize = 64 * 1024;
    gf.fiber = co_create(kHostStackSize, &FiberProcTrampoline);

    if (!gf.fiber) {
        RT_LOG(RT_TAG_OS) << "co_create failed for thread 0x"
                  << std::hex << guestThreadAddr << std::dec << std::endl;
        return false;
    }
#endif

    s_fibers[guestThreadAddr] = gf;
    
    
    return true;
}

void GuestFiberManager::ResumeGuestThread(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fibers.find(guestThreadAddr);
    if (it == s_fibers.end()) {
        RT_LOG(RT_TAG_OS) << "ResumeGuestThread: no fiber for 0x"
                  << std::hex << guestThreadAddr << std::dec << std::endl;
        return;
    }
    
    if (it->second.terminated) {
        RT_LOG(RT_TAG_OS) << "ResumeGuestThread: thread 0x"
                  << std::hex << guestThreadAddr << " already terminated" << std::dec << std::endl;
        return;
    }
    
    it->second.state = ThreadState::READY;

}

void GuestFiberManager::SuspendGuestThread(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    auto it = s_fibers.find(guestThreadAddr);
    if (it == s_fibers.end()) {
        return;
    }
    
    it->second.state = ThreadState::WAITING;
    
}

void GuestFiberManager::ExitGuestThread(uint32_t guestThreadAddr, ThreadState finalState) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    auto it = s_fibers.find(guestThreadAddr);
    if (it == s_fibers.end()) {
        return;
    }
    
    it->second.state = finalState;
    it->second.terminated = true;
    if (s_currentGuestThread == guestThreadAddr) {
        s_currentGuestThread = 0;
    }
    
    if (it->second.fiber && !it->second.isSchedulerFiber) {
#if defined(_WIN32)
        const void* current = GetCurrentFiber();
        if (it->second.fiber == current) {
            s_fibersPendingDelete.push_back(it->second.fiber);
        } else {
            DeleteFiber(it->second.fiber);
        }
#else
        const void* current = co_active();
        if (it->second.fiber == current) {
            // Deleting the coroutine we're currently executing on would free the very stack
            // this call is running on; defer it (PurgePendingFibers) until some other fiber is
            // active, exactly like the Windows branch above.
            s_fibersPendingDelete.push_back(it->second.fiber);
        } else {
            co_delete(static_cast<cothread_t>(it->second.fiber));
        }
#endif
        it->second.fiber = nullptr;
    }
}

void GuestFiberManager::SwitchToThread(uint32_t guestThreadAddr, CpuContext* cpu) {
    PurgePendingFibers();
    if (!s_initialized) {
        RT_LOG(RT_TAG_OS) << "SwitchToThread called before initialization!" << std::endl;
        return;
    }
    
    void* fiberHandle = nullptr;
    uint32_t previousThread = 0;
    CpuContext callerContext{};
    const bool haveCallerContext = (cpu != nullptr);
    CpuContext targetContext{};
    bool haveTargetContext = false;

    if (cpu) {
        callerContext = *cpu;
    }
    
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        
        auto it = s_fibers.find(guestThreadAddr);
        if (it == s_fibers.end()) {
            RT_LOG(RT_TAG_OS) << "SwitchToThread: no fiber for 0x"
                      << std::hex << guestThreadAddr << std::dec << std::endl;
            return;
        }
        
        fiberHandle = it->second.fiber;
        
        if (!fiberHandle || it->second.terminated) {
            RT_LOG(RT_TAG_OS) << "SwitchToThread: invalid fiber for 0x"
                      << std::hex << guestThreadAddr << std::dec << std::endl;
            return;
        }
        
        // Remember what thread we're switching from
        previousThread = s_currentGuestThread;
        
        // Save current thread's CPU context if switching from a guest thread
        if (s_currentGuestThread != 0 && cpu) {
            auto currentIt = s_fibers.find(s_currentGuestThread);
            if (currentIt != s_fibers.end()) {
                currentIt->second.cpuContext = *cpu;
            }
        }
        
        // Update current thread
        s_currentGuestThread = guestThreadAddr;
        it->second.state = ThreadState::RUNNING;

        if (cpu) {
            targetContext = it->second.cpuContext;
            haveTargetContext = true;
        }
    }
    
    // Store CPU context pointer for the target fiber to use
    s_cpuContext = cpu;
    
    // Check if we're already on the target fiber (e.g., switching to main thread
    // when we're already on the scheduler fiber)
#if defined(_WIN32)
    void* currentFiber = GetCurrentFiber();
#else
    void* currentFiber = co_active();
#endif
    if (currentFiber == fiberHandle) {
        // Already executing on the target host fiber. This is common for the
        // default guest thread, which also owns the scheduler fiber. Keep the
        // live CPU context instead of restoring a possibly stale saved copy
        // from before the guest thread slept.
        return;
    }

    if (cpu && haveTargetContext) {
        *cpu = targetContext;
        // FPSCR travels with the guest-thread context, and its NI bit is
        // modeled through the per-host-thread MXCSR; every context restore
        // must re-mirror it.
        MkwApplyHostNiMode(cpu->fpscr);
    }

    // Switch to the target fiber (the target fiber will load its own context)
#if defined(_WIN32)
    SwitchToFiber(fiberHandle);
#else
    // Staged for FiberProcTrampoline's first (and only) read; a no-op for a fiber that has
    // already started, since resuming it re-enters mid-function rather than through the
    // trampoline's entry point.
    s_pendingFiberArg = guestThreadAddr;
    co_switch(static_cast<cothread_t>(fiberHandle));
#endif

    // When we return here, the fiber that issued SwitchToThread has resumed.
    // That does not automatically mean the previous guest thread became runnable
    // again; a different thread may simply have yielded back to the scheduler.
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        uint32_t runningContext = 0;
        uint32_t currentContext = 0;
        try {
            runningContext = Memory::Read32(kOSRunningContextAddr);
            currentContext = Memory::Read32(kOSCurrentContextAddr);
        } catch (const Memory::AccessViolation&) {
            runningContext = 0;
            currentContext = 0;
        }

        const bool resumedPreviousThread =
            previousThread != 0 &&
            runningContext == previousThread &&
            currentContext == previousThread;

        if (resumedPreviousThread) {
            auto it = s_fibers.find(previousThread);
            if (it != s_fibers.end()) {
                if (cpu) {
                    *cpu = it->second.cpuContext;
                    MkwApplyHostNiMode(cpu->fpscr);
                }
                it->second.state = ThreadState::RUNNING;
            }
            s_currentGuestThread = previousThread;
        } else {
            if (cpu && haveCallerContext) {
                *cpu = callerContext;
                MkwApplyHostNiMode(cpu->fpscr);
            }
            s_currentGuestThread = 0;
        }
    }
}

uint32_t GuestFiberManager::GetCurrentGuestThread() {
    return s_currentGuestThread;
}

GuestFiber* GuestFiberManager::GetFiber(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fibers.find(guestThreadAddr);
    return it != s_fibers.end() ? &it->second : nullptr;
}

bool GuestFiberManager::HasFiber(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_fibers.count(guestThreadAddr) > 0;
}

bool GuestFiberManager::IsTerminated(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fibers.find(guestThreadAddr);
    return it != s_fibers.end() && it->second.terminated;
}

bool GuestFiberManager::RegisterMainThreadAsFiber(uint32_t guestThreadAddr, CpuContext* cpu) {
    if (!s_initialized) {
        RT_LOG(RT_TAG_OS) << "RegisterMainThreadAsFiber called before initialization!" << std::endl;
        return false;
    }
    
    std::lock_guard<std::mutex> lock(s_mutex);
    
    // Check if already registered
    if (s_fibers.count(guestThreadAddr)) {
        return true;
    }
    
    // Register the current host fiber (scheduler fiber) as this guest thread's fiber
    GuestFiber gf;
    gf.fiber = s_schedulerFiber;  // The main fiber IS the scheduler fiber
    gf.entryPoint = 0;
    gf.entryArg = 0;
    gf.state = ThreadState::RUNNING;
    gf.terminated = false;
    gf.isSchedulerFiber = true;  // This is special - it's both scheduler AND main thread
    
    // Copy current CPU context
    if (cpu) {
        gf.cpuContext = *cpu;
    }
    
    s_fibers[guestThreadAddr] = gf;
    s_currentGuestThread = guestThreadAddr;
    
    return true;
}

void GuestFiberManager::ProcessTimerEvents(CpuContext* cpu) {
    constexpr uint32_t kMaxRetracesPerSlice = 16;
    const uint32_t pending = g_viRetracePendingCount.exchange(0, std::memory_order_acq_rel);
    if (pending == 0) {
        return;
    }

    const uint32_t retracesToProcess = std::min(pending, kMaxRetracesPerSlice);
    const uint32_t remaining = pending - retracesToProcess;
    if (remaining != 0) {
        g_viRetracePendingCount.fetch_add(remaining, std::memory_order_release);
    }

    for (uint32_t i = 0; i < retracesToProcess; ++i) {
        VI_HLE_ForceRetrace(cpu);
    }
}

void GuestFiberManager::SwitchToScheduler() {
#if defined(_WIN32)
    SwitchToFiber(s_schedulerFiber);
#else
    co_switch(static_cast<cothread_t>(s_schedulerFiber));
#endif
}

#if defined(_WIN32)
void CALLBACK GuestFiberManager::FiberProc(void* param)
#else
void GuestFiberManager::FiberProc(void* param)
#endif
{
    uint32_t guestThreadAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));

    // Get our fiber info
    GuestFiber* fiber = nullptr;
    uint32_t entryPoint = 0;
    uint32_t entryArg = 0;
    
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_fibers.find(guestThreadAddr);
        if (it == s_fibers.end()) {
            RT_LOG(RT_TAG_OS) << "FiberProc: fiber not found!" << std::endl;
            SwitchToScheduler();
            return;
        }
        fiber = &it->second;
        entryPoint = fiber->entryPoint;
        entryArg = fiber->entryArg;
    }
    
    // Get the CPU context
    CpuContext* cpu = s_cpuContext;
    if (!cpu) {
        cpu = &GetPersistentCpuContext();
    }
    
    // Guest OSContext: r2 (TOC/SDA2) at 0x08, r13 (SDA) at 0x34. Both must load correctly or
    // translated code loses access to global/static data.
    try {
        // Load r2 (TOC/SDA2 pointer)
        cpu->gpr[2] = Memory::Read32(guestThreadAddr + 0x08u);
        // Load r13 (SDA pointer) - this is CRITICAL for global data access
        cpu->gpr[13] = Memory::Read32(guestThreadAddr + 0x34u);
        // Load stack pointer from guest context
        cpu->gpr[1] = Memory::Read32(guestThreadAddr + 0x04u);
        // Load saved LR
        cpu->lr = Memory::Read32(guestThreadAddr + 0x84u);
        // Force this region's SDA bases after load. OSInitContext / stale
        // template contexts can leave the opposite region's pointers here
        // (PAL 8038EFA0/8038CC00 vs USA 8038AC20/80388880), which makes
        // every translated global access fault or read garbage.
        cpu->gpr[2] = RuntimeConfig::SDA2_BASE;
        cpu->gpr[13] = RuntimeConfig::SDA1_BASE;

    } catch (const Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "Failed to load guest context from 0x" << std::hex << guestThreadAddr
                  << ": " << e.what() << std::dec << std::endl;
    }
    
    // Set up context for thread entry
    cpu->gpr[3] = entryArg;
    cpu->pc = entryPoint;
    cpu->srr0 = entryPoint;
    
    // Create a CpuContextScope for this fiber
    CpuContextScope scope(cpu);

    // Snapshot the fiber's own entry register state NOW, before the deferral
    // loop below yields: SwitchToScheduler resumes with the shared CpuContext
    // holding a different guest fiber's registers, and the entry trampoline
    // (r3=obj, r13=SDA, r2, r1, lr) must be the fiber's own, not leftover
    // state from whatever ran while this fiber yielded. Snapshot the FULL
    // register file, not just the entry contract: translated callees spill
    // resident locals into the shared context's nonvolatiles, and the entry
    // call below (InvokeIndirectCpu -> guards save/restore around it) would
    // otherwise restore another fiber's residues on top of this one.
    // NOTE: a plain struct copy is NOT enough: PPC_FPR is a union and the
    // shared context's fpr slots may hold signaling NaNs / stale garbage
    // from another fiber. Copy field-by-field through the double member so
    // every slot is a well-defined value.
    CpuContext entryCpu;
    entryCpu.pc = cpu->pc;
    for (int i = 0; i < 32; ++i) entryCpu.gpr[i] = cpu->gpr[i];
    entryCpu.cr = cpu->cr;
    entryCpu.lr = cpu->lr;
    entryCpu.ctr = cpu->ctr;
    entryCpu.xer = cpu->xer;
    entryCpu.fpscr = cpu->fpscr;
    entryCpu.srr0 = cpu->srr0;
    entryCpu.srr1 = cpu->srr1;
    entryCpu.msr = cpu->msr;
    for (int i = 0; i < 32; ++i) entryCpu.fpr[i].d = cpu->fpr[i].d;
    for (int i = 0; i < 8; ++i) entryCpu.gqr[i] = cpu->gqr[i];
    entryCpu.hid0 = cpu->hid0;
    entryCpu.hid1 = cpu->hid1;
    entryCpu.hid2 = cpu->hid2;
    
    int startDeferAttempts = 0;
    uint32_t lastDeferVtable = 0;
    uint32_t lastDeferStartFn = 0;
    while (entryPoint == 0x8024373c) { // EGG::Thread::start
        uint32_t vtable = 0;
        uint32_t startFn = 0;
        try {
            vtable = Memory::Read32(entryArg);
            if (vtable >= 0x80000000u) {
                startFn = Memory::Read32(vtable + 0x0Cu);
            }
        } catch (const Memory::AccessViolation&) {
            vtable = 0;
            startFn = 0;
        }
        lastDeferVtable = vtable;
        lastDeferStartFn = startFn;

        // Break only once the *derived* vtable is installed. At OSCreateThread
        // time the object may still carry the base EGG::Thread vtable
        // (0x802A3FC0), whose slot+12 is the no-op base run (0x8024374c):
        // starting the fiber then runs base run, returns immediately, and the
        // fiber is reaped as a dead detached thread before the derived
        // constructor installs the real run target (e.g. DiscCheckThread::run
        // 0x80008D18). The DvdThread then never polls GetDriveStatus and the
        // scene parks in the disc-error branch forever. Yield until the slot
        // leaves the base run; a thread genuinely using the base run just
        // waits out the retry budget below and exits as before.
        constexpr uint32_t kEggThreadBaseVtable = 0x802A3FC0u;
        constexpr uint32_t kEggThreadBaseRun = 0x8024374cu;
        const bool derivedInstalled = (vtable != kEggThreadBaseVtable) ||
            (startFn != kEggThreadBaseRun);
        if (vtable >= 0x80000000u && startFn >= 0x80000000u && derivedInstalled) {
            break;
        }

        if (startDeferAttempts++ > 50) {
            RT_LOG(RT_TAG_OS) << "EGG::Thread::start target still invalid (vtable=0x" << std::hex << vtable
                      << ", fn=0x" << startFn << ") after retries; continuing anyway." << std::dec << std::endl;
            break;
        }
        SwitchToScheduler();
    }

    // The deferral loop above yields to the scheduler and therefore resumes
    // with registers from a different guest fiber in the shared CpuContext.
    // Restore the fiber's own entry state snapshotted above (field-by-field
    // through the double member, mirroring the snapshot), then apply the
    // entry contract on top (r3=obj, pc/srr0=entry).
    cpu->pc = entryCpu.pc;
    for (int i = 0; i < 32; ++i) cpu->gpr[i] = entryCpu.gpr[i];
    cpu->cr = entryCpu.cr;
    cpu->lr = entryCpu.lr;
    cpu->ctr = entryCpu.ctr;
    cpu->xer = entryCpu.xer;
    cpu->fpscr = entryCpu.fpscr;
    cpu->srr1 = entryCpu.srr1;
    cpu->msr = entryCpu.msr;
    for (int i = 0; i < 32; ++i) cpu->fpr[i].d = entryCpu.fpr[i].d;
    for (int i = 0; i < 8; ++i) cpu->gqr[i] = entryCpu.gqr[i];
    cpu->hid0 = entryCpu.hid0;
    cpu->hid1 = entryCpu.hid1;
    cpu->hid2 = entryCpu.hid2;
    cpu->gpr[3] = entryArg;
    cpu->pc = entryPoint;
    cpu->srr0 = entryPoint;

    
    // Call the translated thread entry function
    const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(entryPoint);
    if (info) {
        // Temporary: name the EGG start target the fiber is about to run
        // (derived vtable's Run vs the base no-op), so the parked-DvdThread
        // trace shows whether the deferral loop above actually waited for
        // the derived vtable. Remove with the GX counters.
        if (entryPoint == 0x8024373cu) {
            RT_LOG(RT_TAG_OS) << "FiberProc: thr=0x" << std::hex << guestThreadAddr
                      << " EGG::start obj=0x" << entryArg
                      << " vt=0x" << lastDeferVtable
                      << " run=0x" << lastDeferStartFn
                      << " r13=0x" << cpu->gpr[13]
                      << " r1=0x" << cpu->gpr[1]
                      << " deferrals=" << std::dec << startDeferAttempts << std::endl;
        }
        // Temporary: trace whether the entry call is even reached. The
        // watchdog shows the fiber entry RETURNED with the derived vtable
        // installed, yet the dth counter never moves — this line proves the
        // dispatch call itself executes. Remove with the GX counters.
        if (entryPoint == 0x8024373cu) {
            RT_LOG(RT_TAG_OS) << "FiberProc: DISPATCH EGG::start thr=0x" << std::hex << guestThreadAddr
                      << " run=0x" << lastDeferStartFn << std::dec << std::endl;
        }
        // Temporary: catch a C++ exception escaping the translated entry.
        // Translated bodies must never throw, but a Memory::AccessViolation
        // (or std::bad_alloc, etc.) would unwind straight through FiberProc
        // and silently kill the fiber — exactly the observed shape (entry
        // logged, nothing after, fiber dead). Log-and-swallow here to prove
        // or rule it out. Remove with the GX counters.
        //
        // ALSO temporary: bypass InvokeIndirectCpu's registry lookup and call
        // the translated body directly. The dispatch counters prove nothing
        // reaches DvdThread_main through the registry path; a direct call
        // distinguishes "registry/dispatch lies" from "body exits instantly".
        // Remove with the GX counters.
        //
        // Implemented WITHOUT a forward declaration (which collides with the
        // TU structure here): resolve the body through the registry's raw
        // pointer and call it directly, skipping only the counting/dispatch
        // wrappers. If this runs the body and the normal path doesn't, the
        // wrappers are at fault; if this also returns instantly, the body is.
        if (entryPoint == 0x8024373cu && lastDeferStartFn == 0x80008D18u) {
            // DvdThread worker: run the REAL trampoline path (EGG::start ->
            // vtable -> DvdThread_main). The direct-body probe scaffolding
            // below is RETIRED (kept for one more run behind this flag):
            // the question it asked is answered (the body runs and parks),
            // and per-activation double-execution skewed every downstream
            // reading. Set kDvdDirectProbe to 1 to re-enable it.
            constexpr bool kDvdDirectProbe = false;
            RT_LOG(RT_TAG_OS) << "FiberProc: thr=0x" << std::hex << guestThreadAddr
                      << " DvdThread via trampoline (obj=0x" << entryArg << ")" << std::dec
                      << std::endl;
            try {
                InvokeIndirectCpu(entryPoint, cpu);
            } catch (const std::exception& e) {
                RT_LOG(RT_TAG_OS) << "FiberProc: thr=0x" << std::hex << guestThreadAddr
                          << " DvdThread trampoline THREW: " << e.what() << std::dec << std::endl;
            } catch (...) {
                RT_LOG(RT_TAG_OS) << "FiberProc: thr=0x" << std::hex << guestThreadAddr
                          << " DvdThread trampoline THREW unknown" << std::dec << std::endl;
            }
            RT_LOG(RT_TAG_OS) << "FiberProc: thr=0x" << std::hex << guestThreadAddr
                      << " DvdThread trampoline returned r3=0x" << cpu->gpr[3] << std::dec
                      << std::endl;
            if (kDvdDirectProbe) {
            // (retired probe body: disabled by the `false` below; the whole
            // block is dead code kept for reference until the GX-counter
            // cleanup. See flag above.)
            const auto* rawRec =
                TranslatedFunctionRegistry::FindRawByAddressPtr(0x80008D18u);
            // Temporary: dump the body's first guest stack frame AFTER
            // the direct call returns (declared function-scope so both
            // branches below can fill it). Remove with the GX counters.
            uint32_t postBodyFrame0 = 0, postBodyFrame1 = 0;
            uint32_t postBodyR1 = cpu->gpr[1];
            if (false) {
                // DvdThread_main by hand is overkill; instead call the entry
                // with a TRACED GetDriveStatus: wrap the raw call so the
                // first InvokeDirectCpu<0x80162B50u> inside the body is
                // visible. The body itself is unmodified — this just proves
                // whether execution reaches past the prologue. Remove with
                // the GX counters.
                RT_LOG(RT_TAG_OS) << "FiberProc: direct body at 0x" << std::hex
                          << reinterpret_cast<uintptr_t>(rawRec->entry) << std::dec << std::endl;
                // Temporary: pre-seed the body's FIRST stack frame manually.
                // The body opens with ResolveRangeHost(r1-48, 56B, write) +
                // writes to [r1-48..r1+4]. If r1 points at an unmapped guard
                // region the writes fault, MemoryInline throws, and the fiber
                // dies silently. Writing the frame marker here first proves
                // or rules out a bad incoming stack pointer. The values are
                // exactly what the prologue would write (backchain + LR
                // slot), so the body re-write is idempotent. Remove with the
                // GX counters.
                try {
                    Memory::Write32(cpu->gpr[1] - 48u, cpu->gpr[1]);
                    RT_LOG(RT_TAG_OS) << "FiberProc: stack probe write ok r1=0x" << std::hex
                              << cpu->gpr[1] << std::dec << std::endl;
                } catch (const std::exception& e) {
                    RT_LOG(RT_TAG_OS) << "FiberProc: stack probe write THREW: " << e.what() << std::endl;
                } catch (...) {
                    RT_LOG(RT_TAG_OS) << "FiberProc: stack probe write THREW unknown" << std::endl;
                }
                // Temporary: ResolveRangeHost pre-check for the body's frame.
                // If the resolver returns null here, the body's prologue
                // throws before executing a single guest instruction.
                // Remove with the GX counters.
                {
                    uint8_t* probe = MemoryInline::ResolveRangeHost(
                        cpu->gpr[1] - 48u, 0, 56u, false, true);
                    RT_LOG(RT_TAG_OS) << "FiberProc: frame resolve "
                              << (probe ? "OK" : "NULL") << " r1=0x" << std::hex
                              << cpu->gpr[1] << std::dec << std::endl;
                }
                // Temporary: call GetDriveStatus DIRECTLY (it is pure
                // translated code, no HLE override). If the direct status
                // call works from here but the body still returns instantly,
                // the body never reaches its own status call. Remove with the
                // GX counters.
                {
                    const auto* stRec =
                        TranslatedFunctionRegistry::FindRawByAddressPtr(0x80162B50u);
                    RT_LOG(RT_TAG_OS) << "FiberProc: status record "
                              << ((stRec && stRec->entry) ? "OK" : "MISSING") << std::endl;
                    if (stRec && stRec->entry) {
                        const uint32_t savedR3 = cpu->gpr[3];
                        const uint32_t savedR1 = cpu->gpr[1];
                        const uint32_t savedR13 = cpu->gpr[13];
                        // UPDATE 7: read the gate the body's way (r13-relative
                        // flat read) IMMEDIATELY before the direct call, with
                        // the same r13 the body will use. If THIS reads 1 and
                        // the call still returns -1, the zero is produced
                        // INSIDE the call (interrupts off/on clobber, or the
                        // status path itself writes the gate). Remove with the
                        // GX counters.
                        //
                        // UPDATE 8: body-way pre-call reads 1, call returns -1.
                        // The ONLY instruction between the gate read and the
                        // -1 return is SetCRResident + the branch test. Either
                        // (a) the gate read inside the call sees a DIFFERENT
                        // value (r13 changed by OSDisableInterrupts? no — it
                        // doesn't touch r13... but UpdateCurrentContextInterruptFlag
                        // WRITES guest memory at [currentContext+0x1A2]! If
                        // currentContext is STALE (points at a recycled struct
                        // overlapping the DVD globals?), that write could
                        // stomp the gate), or (b) SetCRResident/branch is
                        // miscompiled. Test (b) first: replicate the exact
                        // compare+branch here with the same values.
                        uint32_t bodyWayPre = 0xDEADu;
                        try {
                            bodyWayPre = MemoryInline::FlatRead32(cpu->gpr[13] - 26004u);
                        } catch (...) {}
                        RT_LOG(RT_TAG_OS) << "FiberProc: body-way gate pre-call=0x" << std::hex
                                  << bodyWayPre << " r13=0x" << cpu->gpr[13] << std::dec
                                  << std::endl;
                        {
                            uint32_t crCopy = cpu->cr;
                            uint32_t xerCopy = cpu->xer;
                            SetCRResident(crCopy, xerCopy, 0, static_cast<int32_t>(bodyWayPre),
                                          static_cast<int32_t>(0));
                            const bool eqTaken = (crCopy & 0x20000000u) != 0;
                            RT_LOG(RT_TAG_OS) << "FiberProc: replica cmp: cr=0x" << std::hex
                                      << crCopy << " eqTaken=" << eqTaken << std::dec << std::endl;
                        }
                        // UPDATE 9: replica cmp gives cr=0x40000000 (GT set,
                        // EQ clear) — CORRECT for 1 vs 0. So the compare and
                        // branch logic are fine, and the body's own gate read
                        // must ALSO see 1... yet the body returns -1. The ONLY
                        // remaining difference between the replica and the
                        // body: the body runs OSDisableInterrupts FIRST (via
                        // InvokeDirectCpu<0x801A65ACu>), and the gate read
                        // happens AFTER. If OSDisableInterrupts (or rather
                        // UpdateCurrentContextInterruptFlag inside it)
                        // CLOBBERS r13 or the gate word, the body's read sees
                        // garbage. Replicate the HLE body inline here (it only
                        // flips a host flag + writes one guest halfword) and
                        // re-read the gate the body way immediately after.
                        // (Can't call the HLE functions directly: their
                        // extern "C" declarations collide with this TU's
                        // namespace structure.)
                        {
                            const uint32_t r13pre = cpu->gpr[13];
                            uint32_t curCtx = 0;
                            (void)MemoryInline::TryReadGuestScalar(0x800000D4u, curCtx);
                            if (curCtx != 0) {
                                uint16_t mf = 0;
                                if (MemoryInline::TryReadGuestScalar(curCtx + 0x1A2u, mf)) {
                                    mf = static_cast<uint16_t>(mf & ~0x0002u);
                                    MemoryInline::TryWriteGuestScalar(curCtx + 0x1A2u, mf);
                                }
                            }
                            uint32_t bodyWayPost = 0xDEADu;
                            try {
                                bodyWayPost = MemoryInline::FlatRead32(cpu->gpr[13] - 26004u);
                            } catch (...) {}
                        // UPDATE 10: post-disable gate STILL 1, r13 untouched,
                        // curCtx = the DvdThread itself. OSDisableInterrupts is
                        // innocent. Remaining suspect inside the direct status
                        // call: OSSleepThread? No — GetDriveStatus never
                        // sleeps. What DOES run inside: InvokeDirectCpu's
                        // PpcNonvolatileGprGuard (saves/restores r14-r31
                        // around the call — harmless)... and
                        // ApplyRuntimeCallOptions (counter bump — harmless).
                        // Then the body: prologue writes, OSDisableInterrupts
                        // (proven innocent), gate read... The gate read MUST
                        // see 1. Unless the body's r13 is NOT 0x8038CC00 at
                        // that point: the direct status call restores r13
                        // from savedR13 AFTER the call — but DURING the call
                        // the body itself reloads r13 from ctx AFTER its own
                        // InvokeDirectCpu<OSDisableInterrupts> returns... and
                        // THAT inner call's guard/scope dance could leave a
                        // STALE r13 in ctx if CpuContextScope restores the
                        // wrong previous_. Nested scopes: FiberProc's scope
                        // (previous_=main's cpu) -> ... The inner
                        // InvokeDirectCpu creates ANOTHER CpuContextScope on
                        // the SAME cpu pointer: previous_ = cpu (itself!),
                        // destructor restores g_currentCpuContext = cpu
                        // (fine) — but PpcNonvolatileGprGuard saves r14-r31,
                        // NOT r13! And KnownNativeCpuCall path ALSO wraps
                        // with PpcNonvolatileGprGuard... r13 is VOLATILE in
                        // the PPC ABI (r13 is reserved/SDA — actually r13 IS
                        // reserved, NOT volatile!). Hmm, r13 is the SDA base:
                        // call-clobbered or not, SOME nested call between the
                        // gate seed and the gate read clobbers it. Dump r13
                        // at THREE points: before direct call, inside (can't),
                        // after. After==before (proven: r13after=0x8038CC00).
                        // So r13 is FINE across the whole direct call. The
                        // gate is 1 before. The call returns -1. CONTRADICTION
                        // unless the -1 comes from somewhere OTHER than the
                        // first gate. RE-READ the body: -1 (r30=-1) is ALSO
                        // set at... only loc_80162B78. And loc_80162B78 is
                        // reached ONLY when cr-EQ is CLEAR after comparing
                        // gate vs 0. Gate=1 -> SetCRResident(cr,1,0) -> GT set,
                        // EQ CLEAR -> branch NOT taken... wait, re-read:
                        // "if ((cr & 0x20000000u) != 0) goto loc_80162B80" —
                        // taken when EQ SET. Gate=1: EQ CLEAR -> FALL THROUGH
                        // to loc_80162B78: r30 = -1!!! THE BRANCH IS INVERTED
                        // FROM WHAT I ASSUMED. cmpne-vs-0-falls-through means
                        // NONZERO gate -> -1 PATH. Let me recheck: BEQ target
                        // is 0x80162B80 (the NEXT gate). Gate=1 (nonzero):
                        // EQ clear -> do NOT branch -> fall into r30=-1,
                        // return -1. GATE=0: EQ set -> branch to next gate.
                        // SO -1 MEANS "GATE NONZERO" — DRIVE BUSY. And gate=1
                        // (seeded "idle+ready") gives -1/BUSY?! The seed
                        // values are WRONG: -26004=1 must mean BUSY (or "a
                        // command is active"), and the drive goes ready when
                        // it reads 0! DVDInit's "seed both nonzero" comment
                        // has the polarity BACKWARDS for -26004.
                        RT_LOG(RT_TAG_OS) << "FiberProc: post-disable gate=0x" << std::hex
                                  << bodyWayPost << " r13pre=0x" << r13pre
                                  << " r13post=0x" << cpu->gpr[13]
                                  << " curCtx=0x" << curCtx << std::dec << std::endl;
                        }
                        try {
                            stRec->entry(cpu);
                            RT_LOG(RT_TAG_OS) << "FiberProc: direct GetDriveStatus returned r3=0x"
                                      << std::hex << cpu->gpr[3]
                                      << " r13after=0x" << cpu->gpr[13]
                                      << " r1after=0x" << cpu->gpr[1] << std::dec << std::endl;
                        } catch (const std::exception& e) {
                            RT_LOG(RT_TAG_OS) << "FiberProc: direct GetDriveStatus THREW: "
                                      << e.what() << std::endl;
                        } catch (...) {
                            RT_LOG(RT_TAG_OS) << "FiberProc: direct GetDriveStatus THREW unknown"
                                      << std::endl;
                        }
                        cpu->gpr[3] = savedR3;
                        cpu->gpr[1] = savedR1;
                        cpu->gpr[13] = savedR13;
                    }
                }
                // Temporary: enter the BODY with its r1 already consumed.
                // The body opens with r1 -= 48 and immediately calls
                // GetDriveStatus — which itself does r1 -= 16. If EITHER
                // frame underflows the stack (guard page / unmapped), the
                // throw happens before any guest-visible work. Pre-extend
                // r1 by the combined 64 bytes here: if the body then runs,
                // the incoming stack pointer was the killer. Remove with the
                // GX counters.
                //
                // DISABLED for this run (set to 0): the pre-extension moves
                // r1 but the body's prologue writes its frame at the MOVED
                // r1, so the frame dump below reads the wrong address. Keep
                // r1 untouched to get a clean frame read.
                // cpu->gpr[1] -= 64u;
                // RT_LOG(RT_TAG_OS) << "FiberProc: pre-extended r1 to 0x" << std::hex
                //           << cpu->gpr[1] << std::dec << std::endl;
                // Temporary: dump the DiscCheckThread object words the body
                // itself reads/writes: +72 (GetDriveStatus result store),
                // +80/+81 (the Run disc gate). Read BEFORE and AFTER the
                // direct call: if +72 changes, the body reached its first
                // status poll; if +80/+81 change, it reached the gate logic.
                // Remove with the GX counters.
                postBodyR1 = cpu->gpr[1];
                uint32_t dcPre72 = 0, dcPre80 = 0, dcPre81 = 0;
                try {
                    dcPre72 = Memory::Read32(entryArg + 72u);
                    dcPre80 = Memory::Read32(entryArg + 80u);
                    dcPre81 = Memory::Read8(entryArg + 81u);
                } catch (...) {}
                RT_LOG(RT_TAG_OS) << "FiberProc: DvdThread obj pre: +72=0x" << std::hex
                          << dcPre72 << " +80=0x" << dcPre80 << " +81=0x" << dcPre81
                          << std::dec << std::endl;
                try {
                    rawRec->entry(cpu);
                } catch (const std::exception& e) {
                    RT_LOG(RT_TAG_OS) << "FiberProc: direct DvdThread_main THREW: " << e.what() << std::endl;
                } catch (...) {
                    RT_LOG(RT_TAG_OS) << "FiberProc: direct DvdThread_main THREW unknown" << std::endl;
                }
                try {
                    postBodyFrame0 = Memory::Read32(postBodyR1 - 48u);
                    postBodyFrame1 = Memory::Read32(postBodyR1 - 44u);
                } catch (...) {}
            } // end retired kDvdDirectProbe block (see flag above)
        } // end DvdThread EGG-start branch
        // (retired probe scaffolding deleted; the trampoline path above is
        // the live DvdThread entry. The neverStarted/RETURNED handling below
        // is shared by all EGG::start fibers.)
        // Temporary: did the entry return (base no-op run) or switch away
            // Temporary: post-call object words (see pre-call read above).
            // Remove with the GX counters.
            try {
                const uint32_t dcPost72 = Memory::Read32(entryArg + 72u);
                const uint32_t dcPost80 = Memory::Read32(entryArg + 80u);
                const uint32_t dcPost81 = Memory::Read8(entryArg + 81u);
                RT_LOG(RT_TAG_OS) << "FiberProc: DvdThread obj post: +72=0x" << std::hex
                          << dcPost72 << " +80=0x" << dcPost80 << " +81=0x" << dcPost81
                          << std::dec << std::endl;
            } catch (...) {}
            // Temporary: re-enter the fiber's OWN entry (the EGG::start
            // trampoline) after the direct body call, so the trampoline's
            // jump runs with the object fully polled (+72=5). On the
            // re-entry the deferral loop breaks immediately (derived vtable
            // is installed) and the trampoline jumps to DvdThread_main for
            // its second iteration — which parks in VIWaitForRetrace now
            // that the retrace path advances one boundary per call. This
            // keeps the DvdThread alive across frames instead of returning
            // after one poll. Remove with the GX counters (the real fix is
            // making the normal dispatch path reach the body).
            //
            // NO — simpler and closer to hardware: just keep calling the
            // body directly while it keeps parking. Each body call now ends
            // in VIWaitForRetrace (one boundary per call, returns after
            // advancing), so looping the body IS the thread's main loop.
            // Cap the iterations so one fiber activation can't monopolize
            // the scheduler; the fiber gets re-entered every frame anyway.
            //
            // UPDATE: the keep-loop runs exactly once (+72=5 already set by
            // the first direct call, so the loop body never re-executes).
            // The thread is alive and parked, but the SCENE still takes the
            // disc-error branch (g81=1). The DvdThread's job is therefore
            // NOT just polling GetDriveStatus — something else must observe
            // the polled state and clear the Run gate. Keep the single poll
            // (it seeds +72/+80/+81) and let the scene tell us what else it
            // waits for. The keep-loop stays but with a zero cap: it now
            // only logs.
            //
            // UPDATE 2: the scene gate is NOT +81 — it is a VALUE the scene
            // reads at sStatic+80 (g81 = (word >> 16) & 0xFF = byte +81).
            // The poll loop's exit condition (+72==5) fires on the FIRST
            // iteration because GetDriveStatus returns -1 (busy), and the -1
            // path writes +72=5 ("not ready, keep waiting") — which our
            // keep-loop mistakes for "drive ready". The REAL ready signal is
            // GetDriveStatus returning 0/1 with the cover state settled.
            // Loop until the STATUS RETURN (not +72) indicates the drive is
            // actually ready: keep polling while ret == -1 or 8.
            //
            // UPDATE 3: 200 polls all return -1, and -1 comes from the FIRST
            // gate (-26004 == 0, "drive busy"), NOT from the waiting queue.
            // DVDInit seeds -26004=1, so something zeroes it between the seed
            // and the first poll. The writer scan shows the ONLY -26004
            // writer in translated code is cbForStateError's error path
            // (func_8015EE70: writes -26004=1 actually — sets busy, not
            // clears). So the zero must come from the HOST side: the
            // CompleteDvdCancelState zero at 0x80386668 (-26008), or... wait,
            // -26004 is 0x8038666C, seeded 1 at DVDInit line 927 AND line 927
            // runs on EVERY DVDInit call — but DVDInit early-returns when
            // g_dvdInitialized. UNLESS the DvdThread fiber ran BEFORE DVDInit
            // completed (it didn't — fiber runs after boot). Read the actual
            // gate words here to see which one is zero, instead of guessing.
            //
            // UPDATE 4: gates read -26004=1, -26008=1 — BOTH SEEDED — yet the
            // very next GetDriveStatus call returns -1, which ONLY happens
            // when the -26004 read inside the body sees 0. The gate read and
            // the body's gate read disagree: the body's r13 is STALE. The
            // direct status call above saves/restores r13 around the call,
            // but the restore uses savedR13 — captured from cpu->gpr[13] at
            // FiberProc time. If the body's GetDriveStatus path CLOBBERED
            // r13 (or the save/restore itself is lossy), every subsequent
            // poll reads the wrong address. Dump r13 before/after the direct
            // call to prove it.
            {
                uint32_t gateA = 0xDEADu, gateB = 0xDEADu, gateC = 0xDEADu;
                try {
                    gateA = Memory::Read32(0x8038666Cu);
                    gateB = Memory::Read32(0x80386668u);
                    gateC = Memory::Read32(0x80386670u - 0xCu + 0xCu);
                } catch (...) {}
                RT_LOG(RT_TAG_OS) << "FiberProc: DVD gates pre-poll: -26004=0x" << std::hex
                          << gateA << " -26008=0x" << gateB << std::dec << std::endl;
                RT_LOG(RT_TAG_OS) << "FiberProc: r13 fiber=0x" << std::hex << cpu->gpr[13]
                          << " SDA1=0x8038CC00" << std::dec << std::endl;
                // UPDATE 5: r13 is CORRECT (0x8038CC00) and both gates read 1
                // via Memory::Read32 — yet the body's FlatRead32 sees 0. The
                // difference between the two reads: Memory::Read32 (checked,
                // goes through the page table) vs MemoryInline::FlatRead32
                // (raw host pointer, byte-swapped). If the FLAT VIEW of that
                // page is stale (page committed but flat mapping not updated,
                // or the write went to a different backing store), the flat
                // read returns the pre-seed zero while the checked read
                // returns 1. Read the same address BOTH ways here to prove it.
                //
                // UPDATE 6: checked=flat=1. Both host reads agree the gate is
                // 1. So the body's FlatRead32(r13 + -26004) MUST also see 1
                // ... unless r13 is not what we think INSIDE the body. The
                // direct status call saves/restores r13 around the call, but
                // the FIRST direct call (the probe above, "direct
                // GetDriveStatus returned r3=-1") runs with the FIBER's r13 —
                // and GetDriveStatus itself may CLOBBER r13 (it only lists
                // r3/r4/r13/r30/r31 as ABI in/out, and the body spill code
                // restores r13 from ctx AFTER the call — wait, it does:
                // "r13 = ctx->gpr[13]" after InvokeDirectCpu. So r13 survives
                // ... unless the clobber happens INSIDE GetDriveStatus via
                // OSSleepThread/SelectThread switching to ANOTHER fiber that
                // leaves its own r13 in the shared context. The body then
                // resumes with the WRONG r13 and reads gate from a wrong
                // address (=0). Dump r13 immediately after the direct status
                // call returns: if it changed, the fiber switch stole it.
                {
                    uint32_t viaChecked = 0xDEADu, viaFlat = 0xDEADu;
                    try { viaChecked = Memory::Read32(0x8038666Cu); } catch (...) {}
                    try { viaFlat = MemoryInline::FlatRead32(0x8038666Cu); } catch (...) {}
                    RT_LOG(RT_TAG_OS) << "FiberProc: -26004 checked=0x" << std::hex
                              << viaChecked << " flat=0x" << viaFlat << std::dec << std::endl;
                }
                int dvdKeeps = 0;
                for (;;) {
                    const auto* stRec =
                        TranslatedFunctionRegistry::FindRawByAddressPtr(0x80162B50u);
                    if (!stRec || !stRec->entry) break;
                    const uint32_t savedR3 = cpu->gpr[3];
                    const uint32_t savedR1 = cpu->gpr[1];
                    const uint32_t savedR13 = cpu->gpr[13];
                    int32_t status = -99;
                    try {
                        stRec->entry(cpu);
                        status = static_cast<int32_t>(cpu->gpr[3]);
                    } catch (...) { status = -99; }
                    cpu->gpr[3] = savedR3;
                    cpu->gpr[1] = savedR1;
                    cpu->gpr[13] = savedR13;
                    if (dvdKeeps == 0) {
                        RT_LOG(RT_TAG_OS) << "FiberProc: dvd status poll0 ret=" << status
                                  << std::endl;
                    }
                    // -1 = busy, 8 = not ready: keep polling. Anything else
                    // (0 idle, 1/4/6/11 states) means the drive answered.
                    if (status != -1 && status != 8) {
                        RT_LOG(RT_TAG_OS) << "FiberProc: dvd status settled ret=" << status
                                  << " after " << std::dec << dvdKeeps << " polls" << std::endl;
                        break;
                    }
                    if (++dvdKeeps >= 200) {
                        RT_LOG(RT_TAG_OS) << "FiberProc: dvd status still not ready after 200 polls"
                                  << std::endl;
                        break;
                    }
                    break; // retired: re-polled the body here; now single-poll only
                }
                RT_LOG(RT_TAG_OS) << "FiberProc: dvd status poll loop done" << std::endl;
            }
            // Temporary: THE FIX CANDIDATE — loop the body until its +72
            // gate reads "ready" (5), exactly as the game expects: on
            // hardware the DvdThread runs forever, polling GetDriveStatus
            // each iteration and parking in VIWaitForRetrace between polls.
            // Our fiber entry returns after ONE iteration because the body's
            // VIWaitForRetrace HLE path returns instead of parking (desktop
            // sleep path). Loop here: re-invoke the body while +72 != 5,
            // capped so a wedged drive can't hang the fiber forever.
            // Remove with the GX counters (the real fix belongs in the
            // VIWaitForRetrace/scheduler path).
            //
            // DISABLED: +72==5 after the FIRST poll (the -1/busy path writes
            // it as "keep waiting", not "ready"), so this loop always runs
            // zero iterations. The status-return loop above is the live one.
            {
                RT_LOG(RT_TAG_OS) << "FiberProc: dvd poll loop skipped (+72 gate is write-once)"
                          << std::endl;
            }
        } else {
        try {
            InvokeIndirectCpu(entryPoint, cpu);
        } catch (const std::exception& e) {
            RT_LOG(RT_TAG_OS) << "FiberProc: thr=0x" << std::hex << guestThreadAddr
                      << " entry 0x" << entryPoint << " THREW: " << e.what() << std::dec << std::endl;
        } catch (...) {
            RT_LOG(RT_TAG_OS) << "FiberProc: thr=0x" << std::hex << guestThreadAddr
                      << " entry 0x" << entryPoint << " THREW unknown" << std::dec << std::endl;
        }
        }
        // Temporary: did the entry return (base no-op run) or switch away
        // (real Run never returns — it parks in VIWaitForRetrace)? A return
        // here with the derived vtable installed means the trampoline's
        // vtable read went somewhere unexpected. Remove with the GX counters.
        if (entryPoint == 0x8024373cu) {
            uint32_t postVt = 0, postRun = 0;
            try {
                postVt = Memory::Read32(entryArg);
                if (postVt >= 0x80000000u) postRun = Memory::Read32(postVt + 0x0Cu);
            } catch (const Memory::AccessViolation&) {}
            RT_LOG(RT_TAG_OS) << "FiberProc: thr=0x" << std::hex << guestThreadAddr
                      << " EGG::start RETURNED pc=0x" << cpu->pc
                      << " lr=0x" << cpu->lr
                      << " r1=0x" << cpu->gpr[1]
                      << " r3=0x" << cpu->gpr[3]
                      << " r12=0x" << cpu->gpr[12]
                      << " ctr=0x" << cpu->ctr
                      << " cr=0x" << cpu->cr
                      << " vt=0x" << postVt << " run=0x" << postRun << std::dec << std::endl;
        }
    } else {
        RT_LOG(RT_TAG_OS) << "Thread entry 0x" << std::hex << entryPoint
                  << " not found in registry!" << std::dec << std::endl;
    }
    
    // Thread entry functions normally return into OSExitThread on hardware.
    // Our host fiber call boundary observes the return directly, so complete the
    // guest OSThread lifecycle here before handing control back to the scheduler.
    //
    // Exception: a return from the EGG::Thread::start trampoline (0x8024373c) is
    // the base no-op run (0x8024374c) executing before the derived constructor
    // installed the real run target. That thread was never started: its fiber
    // stays schedulable and its guest OSThread MUST keep its READY state, or
    // SelectThread (which picks by guest state) will never start it once the
    // derived vtable is installed. Only a return from a real entry retires the
    // thread with the detach/MORIBUND lifecycle.
    const bool neverStarted = (entryPoint == 0x8024373cu);

    if (!neverStarted) {
        try {
            RemoveGuestThreadFromQueue(guestThreadAddr);
            const uint16_t attributes = Memory::Read16(guestThreadAddr + kThreadAttrOffset);
            const bool detached = (attributes & 1u) != 0;
            const uint16_t finalState = detached ? 0u : static_cast<uint16_t>(ThreadState::MORIBUND);
            if (!detached) {
                Memory::Write32(guestThreadAddr + kThreadExitValueOffset, 0);
            }
            Memory::Write16(guestThreadAddr + kThreadStateOffset, finalState);
            WakeGuestThreadsOnQueueNoSwitch(guestThreadAddr + kThreadJoinQueueOffset);
            if (Memory::Read32(kOSRunningContextAddr) == guestThreadAddr) {
                Memory::Write32(kOSRunningContextAddr, 0);
            }
            if (Memory::Read32(kOSCurrentContextAddr) == guestThreadAddr) {
                Memory::Write32(kOSCurrentContextAddr, 0);
            }
            Memory::Write32(kSchedulerReschedCounterAddr, 1);
        } catch (const Memory::AccessViolation& e) {
            RT_LOG(RT_TAG_OS) << "Thread return cleanup failed for 0x" << std::hex
                      << guestThreadAddr << " at 0x" << e.address() << std::dec
                      << " (" << e.reason() << ")" << std::endl;
        }
    }

    // A natural return from EGG::Thread::start (0x8024373c) may be the base
    // no-op run (0x8024374c) executing before the derived constructor
    // installed the real run target - the fiber entry stays schedulable so a
    // later SelectThread can start it properly. Only a return from a real
    // (non-start-trampoline) entry retires the fiber; the guest OSThread
    // lifecycle above already ran either way.
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_fibers.find(guestThreadAddr);
        if (it != s_fibers.end() && entryPoint != 0x8024373cu) {
            it->second.terminated = true;
            it->second.state = ThreadState::MORIBUND;
        }
        s_currentGuestThread = 0;
    }

    // Return to scheduler
    SwitchToScheduler();
}

#if !defined(_WIN32)
void GuestFiberManager::FiberProcTrampoline() {
    const uint32_t guestThreadAddr = s_pendingFiberArg;
    FiberProc(reinterpret_cast<void*>(static_cast<uintptr_t>(guestThreadAddr)));
    // FiberProc always calls SwitchToScheduler() on every exit path and never falls off its own
    // end; this is only a safety net in case that ever changes; falling off co_create's entry
    // function is otherwise undefined behavior (libco's own crash() fallback aborts instead).
    SwitchToScheduler();
}
#endif

} // namespace Fiber
