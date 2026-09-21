#include "fiber_manager.h"
#include "memory.h"
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "runtime_log.h"

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
        // Temporary: trace whether the trampoline's jump actually reaches
        // DvdThread_main: the post-call state below shows the fiber entry
        // RETURNED with the derived vtable installed, yet the watchdog's dth
        // counter never moves. Remove with the GX counters.
        if (entryPoint == 0x8024373cu) {
            RT_LOG(RT_TAG_OS) << "FiberProc: DISPATCH EGG::start thr=0x" << std::hex << guestThreadAddr
                      << std::dec << std::endl;
        }
        // Temporary: direct counter witness INSIDE the fiber TU (which is
        // provably fresh in the running exe): +2000000 per EGG::start entry.
        // Watchdog dth therefore reads 2000000*N(entries) + 1000001*M(Run
        // visits): M>0 proves DvdThread_main runs. Remove with the GX counters.
        if (entryPoint == 0x8024373cu) {
            g_sceneChainCallCounts.dvdThread.fetch_add(2000000, std::memory_order_relaxed);
        }
        InvokeIndirectCpu(entryPoint, cpu);
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
