#if defined(__ANDROID__)
#include <jni.h>
#include <filesystem>
#include <string>
#include <SDL3/SDL_main.h>

#include "android_files_dir.h"
#include "hle_stubs.h"
#include "memory.h"
#include "recomp_mod_loader.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int RuntimeMain(int argc, char** argv);

namespace AndroidFilesDir {
namespace {
std::filesystem::path g_path;
}
std::filesystem::path Get() noexcept { return g_path; }
void Set(std::filesystem::path p) noexcept { g_path = std::move(p); }
} // namespace AndroidFilesDir

extern "C" void Android_SetFilesDir(const char* path) {
    if (path) AndroidFilesDir::Set(std::filesystem::path(path));
}

// JNI glue called from GameActivity
static float g_gyroRoll = 0.f;

extern "C" JNIEXPORT void JNICALL
Java_org_patchzyy_wiicompiled_GameActivity_nativeSetFilesDir(JNIEnv* env, jobject, jstring path) {
    if (!path) return;
    const char* c = env->GetStringUTFChars(path, nullptr);
    if (c) { AndroidFilesDir::Set(std::filesystem::path(c)); ::setenv("WIICOMPILED_FILES_DIR", c, 1); env->ReleaseStringUTFChars(path, c); }
}

extern "C" JNIEXPORT void JNICALL
Java_org_patchzyy_wiicompiled_GameActivity_nativeOnGyroSteer(JNIEnv*, jobject, jfloat roll) {
    g_gyroRoll = roll;
}

extern "C" JNIEXPORT void JNICALL
Java_org_patchzyy_wiicompiled_GameActivity_nativeOnBackPressed(JNIEnv*, jobject) {}

extern "C" float Android_GetGyroRoll() { return g_gyroRoll; }

extern "C" JNIEXPORT jint JNICALL
Java_org_patchzyy_wiicompiled_GameActivity_nativeGetGuestExecutionAddress(JNIEnv*, jobject) {
    // Any-thread mirror: this JNI call runs on a Binder/GL thread, not the
    // guest thread that owns the thread_local value.
    return static_cast<jint>(
        RecompMod::g_currentTranslatedExecutionAddressAnyThread.load(std::memory_order_relaxed));
}

// Frame-poller counters for the Java watchdog (see ViState): presented XFB
// frames vs VI retrace ticks. Equal-and-growing = guest alive but nothing
// reaching the screen; both frozen = guest thread wedged. The second value
// is the renderer's own successful-present total: advancing VI presents
// with a frozen renderer total means Present() never succeeds.
extern "C" JNIEXPORT jint JNICALL
Java_org_patchzyy_wiicompiled_GameActivity_nativeGetPresentedFrames(JNIEnv*, jobject) {
    return static_cast<jint>(VI_HLE_PresentedFrames());
}

extern "C" JNIEXPORT jint JNICALL
Java_org_patchzyy_wiicompiled_GameActivity_nativeGetRetraces(JNIEnv*, jobject) {
    return static_cast<jint>(VI_HLE_Retraces());
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_patchzyy_wiicompiled_GameActivity_nativeGetRendererPresents(JNIEnv*, jobject) {
    AuroraPresentTiming timing{};
    aurora_get_present_timing(&timing);
    return static_cast<jlong>(timing.totalPresentCount);
}

// Sealed-frame content diagnostics for the Java watchdog (see g_diag* in
// aurora.cpp): packed as draws|sealed + sizes + present-source WxH. Tells
// "renderer sealing EMPTY frames" apart from "frames full but black".
extern "C" JNIEXPORT jstring JNICALL
Java_org_patchzyy_wiicompiled_GameActivity_nativeGetFrameDiagnostics(JNIEnv* env, jobject) {
    uint64_t draws = 0, vert = 0, uni = 0, tex = 0, sealed = 0;
    uint32_t psw = 0, psh = 0, psxfb = 0;
    uint64_t sdraws = 0, scmds = 0, spasses = 0, ebdraws = 0;
    aurora_get_sealed_frame_diagnostics(&draws, &vert, &uni, &tex, &sealed,
        &psw, &psh, &psxfb, &sdraws, &scmds, &spasses, &ebdraws);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "draws=%llu vert=%llu uni=%llu tex=%llu sealed=%llu ps=%ux%u%s sdraws=%llu scmds=%llu spass=%llu eb=%llu",
        (unsigned long long)draws, (unsigned long long)vert,
        (unsigned long long)uni, (unsigned long long)tex,
        (unsigned long long)sealed, psw, psh, psxfb ? "[xfb]" : "[fb]",
        (unsigned long long)sdraws, (unsigned long long)scmds,
        (unsigned long long)spasses, (unsigned long long)ebdraws);
    return env->NewStringUTF(buf);
}

extern "C" void GX_HLE_DiagSnapshot(uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*);
extern "C" void GX_HLE_DiagFifoSnapshot(uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*,
                                        uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*,
                                        uint64_t*, uint64_t*);
extern "C" void GX_HLE_DiagFifoStallSnapshot(uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*,
                                             uint64_t*, uint64_t*, uint64_t*, uint64_t* = nullptr,
                                             uint64_t* = nullptr);
void OS_HLE_DumpThreadsTemp();
extern "C" void VI_HLE_DiagSnapshot(uint64_t*, uint64_t*, uint64_t*, uint64_t*);

// Cumulative guest GX submission counters (see g_diag* in gx_utils.cpp):
// zero begins/ends/lists while CopyDisps proceed = render thread parked;
// begins without ends or verts = submission path broken.
extern "C" JNIEXPORT jstring JNICALL
Java_org_patchzyy_wiicompiled_GameActivity_nativeGetGxDiagnostics(JNIEnv* env, jobject) {
    uint64_t begins = 0, ends = 0, lists = 0, fifoBytes = 0, dlBegins = 0, dlEnds = 0, dlActive = 0;
    GX_HLE_DiagSnapshot(&begins, &ends, &lists, &fifoBytes, &dlBegins, &dlEnds, &dlActive);
    uint64_t fdraw = 0, frawok = 0, frawfail = 0, fincr = 0, fnull = 0, funk = 0;
    uint64_t fbp = 0, fcp = 0, fxf = 0, fib = 0, vrem = 0, fback = 0, nattr = 0;
    GX_HLE_DiagFifoSnapshot(&fdraw, &frawok, &frawfail, &fincr, &fnull, &funk,
                            &fbp, &fcp, &fxf, &fib, &vrem, &fback, &nattr);
    uint64_t viadv = 0, vipost = 0, viguard = 0, viret = 0;
    VI_HLE_DiagSnapshot(&viadv, &vipost, &viguard, &viret);
    uint64_t stNop = 0, stBp = 0, stCp = 0, stXf = 0, stIndx = 0, stCallDl = 0, stDraw = 0, stAttr = 0;
    uint64_t stBpReg = 0, stBpBack = 0;
    GX_HLE_DiagFifoStallSnapshot(&stNop, &stBp, &stCp, &stXf, &stIndx, &stCallDl, &stDraw, &stAttr,
                                 &stBpReg, &stBpBack);
    char buf[704];
    std::snprintf(buf, sizeof(buf), "gxbeg=%llu gxend=%llu gxdl=%llu gxfifo=%llu dlbeg=%llu dlend=%llu dlact=%llu"
        " fdraw=%llu frawok=%llu frawfail=%llu fincr=%llu fnull=%llu funk=%llu fbp=%llu fcp=%llu fxf=%llu fib=%llu vrem=%llu fback=%llu nattr=%llu"
        " viadv=%llu vipost=%llu viguard=%llu viret=%llu"
        " stNop=%llu stBp=%llu stCp=%llu stXf=%llu stIndx=%llu stDl=%llu stDraw=%llu stAttr=%llu"
        " bpReg=0x%llx bpBack=%llu",
        (unsigned long long)begins, (unsigned long long)ends,
        (unsigned long long)lists, (unsigned long long)fifoBytes,
        (unsigned long long)dlBegins, (unsigned long long)dlEnds,
        (unsigned long long)dlActive,
        (unsigned long long)fdraw, (unsigned long long)frawok, (unsigned long long)frawfail,
        (unsigned long long)fincr, (unsigned long long)fnull, (unsigned long long)funk,
        (unsigned long long)fbp, (unsigned long long)fcp, (unsigned long long)fxf,
        (unsigned long long)fib, (unsigned long long)vrem, (unsigned long long)fback,
        (unsigned long long)nattr,
        (unsigned long long)viadv, (unsigned long long)vipost,
        (unsigned long long)viguard, (unsigned long long)viret,
        (unsigned long long)stNop, (unsigned long long)stBp, (unsigned long long)stCp,
        (unsigned long long)stXf, (unsigned long long)stIndx, (unsigned long long)stCallDl,
        (unsigned long long)stDraw, (unsigned long long)stAttr,
        (unsigned long long)stBpReg, (unsigned long long)stBpBack);
    // Temporary: one-shot guest thread dump per watchdog sample (logcat THRDUMP
    // lines). Remove with the GX counters.
    OS_HLE_DumpThreadsTemp();
    // Temporary: EGG::Thread dispatch state for the never-starting workers.
    // Reads the base no-op Run trampoline target (0x8024374C) and, for the
    // TaskThread objects, the message-queue head: a nonzero queue with a
    // parked worker means jobs were queued but the worker never ran. Remove
    // with the GX counters.
    {
        char tbuf[256];
        uint32_t runTarget = 0;
        if (Memory::TryRead32(0x802A3FC0u + 12u, runTarget)) {
            std::snprintf(tbuf, sizeof(tbuf), " eggRun=0x%08X", runTarget);
        } else {
            std::snprintf(tbuf, sizeof(tbuf), " eggRun=?");
        }
        std::string out(buf);
        out += tbuf;
        for (uint32_t obj : {0x8042BBF0u, 0x8042E930u, 0x804294E4u}) {
            uint32_t qHead = 0, curJob = 0, v = 0, run = 0;
            Memory::TryRead32(obj, v);
            Memory::TryRead32(v + 12u, run);
            Memory::TryRead32(obj + 0x0Cu, qHead);
            Memory::TryRead32(obj + 0x48u, curJob);
            char ebuf[128];
            std::snprintf(ebuf, sizeof(ebuf), " t%08X[v=0x%08X run=0x%08X q=0x%08X cur=0x%08X]",
                obj, v, run, qHead, curJob);
            out += ebuf;
        }
        // Scene chain: sSystem -> +84 SceneMgr -> +12 current scene ->
        // vtable calc/draw targets + strap gate words, plus TaskThread queue
        // depth (head-count + current job) for the strap worker: jobs queued
        // but a parked worker means the worker never ran. Remove with the GX
        // counters.
        {
            uint32_t sSys = 0, mgr = 0, cur = 0, cvt = 0, calcT = 0, drawT = 0;
            uint32_t w3192 = 0, w3264 = 0, w3268 = 0, w3184 = 0, m20 = 0, m28 = 0;
            uint32_t b3276 = 0, b180 = 0, b181 = 0;
            if (Memory::TryRead32(0x80386F60u, sSys) && sSys != 0) {
                Memory::TryRead32(sSys + 84u, mgr);
            }
            if (mgr != 0) {
                Memory::TryRead32(mgr + 12u, cur);
                Memory::TryRead32(mgr + 20u, m20);
                Memory::TryRead32(mgr + 28u, m28);
            }
            if (cur != 0) {
                Memory::TryRead32(cur, cvt);
                Memory::TryRead32(cvt + 12u, calcT);
                Memory::TryRead32(cvt + 16u, drawT);
                Memory::TryRead32(cur + 3192u, w3192);
                Memory::TryRead32(cur + 3264u, w3264);
                Memory::TryRead32(cur + 3268u, w3268);
                Memory::TryRead32(cur + 3184u, w3184);
                uint32_t b = 0;
                if (Memory::TryRead32(cur + 3276u, b)) b3276 = b & 0xFFu;
                if (Memory::TryRead32(cur + 180u, b)) b180 = b & 0xFFu;
                if (Memory::TryRead32(cur + 181u, b)) b181 = b & 0xFFu;
            }
            // Strap worker (obj 0x8042BBF0): +12 queue head/count words, +76
            // job ring base, +80 job count, +48 current job.
            uint32_t q0 = 0, q1 = 0, jb = 0, jc = 0, cj = 0;
            Memory::TryRead32(0x8042BBFCu, q0);
            Memory::TryRead32(0x8042BC00u, q1);
            Memory::TryRead32(0x8042BC3Cu, jb);
            Memory::TryRead32(0x8042BC40u, jc);
            Memory::TryRead32(0x8042BC38u, cj);
            char sbuf[384];
            std::snprintf(sbuf, sizeof(sbuf),
                " scn[sSys=0x%08X mgr=0x%08X cur=0x%08X calc=0x%08X draw=0x%08X"
                " 3192=0x%08X 3264=%u 3268=%u 3184=%u m20=%u m28=%u f3276=%u b180=%u b181=%u"
                " tq[q0=%u q1=%u base=0x%08X n=%u cur=0x%08X]]",
                sSys, mgr, cur, calcT, drawT,
                w3192, w3264, w3268, w3184, m20, m28, b3276, b180, b181,
                q0, q1, jb, jc, cj);
            out += sbuf;
        }
        return env->NewStringUTF(out.c_str());
    }
}

static uint32_t g_discGameCode = 0;
extern "C" uint32_t Android_GetDiscGameCode() { return g_discGameCode; }

static uint32_t ReadDiscGameCode(const std::filesystem::path& romDir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(romDir, ec)) return 0;
    for (auto& e : std::filesystem::directory_iterator(romDir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec) || ec) continue;
        auto sz = e.file_size(ec);
        if (ec || sz < 0x20) continue;
        FILE* f = std::fopen(e.path().string().c_str(), "rb");
        if (!f) continue;
        uint8_t hdr[8] = {};
        size_t n = std::fread(hdr, 1, 8, f);
        std::fclose(f);
        if (n < 4) continue;
        // Skip container magics: WBFS/CISO/RVZ/WIA images don't carry the
        // game ID at offset 0 (raw ISO/GCM do). A missed probe falls back to
        // RMCP until dvd.cpp publishes the real nod header to lowmem.
        auto startsWith = [&](const char* m, size_t len) {
            return n >= len && std::memcmp(hdr, m, len) == 0;
        };
        if (startsWith("WBFS", 4) || startsWith("CISO", 4) ||
            startsWith("RVZ", 3) || startsWith("WIA", 3)) {
            continue;
        }
        bool alpha = true;
        for (int i = 0; i < 4; ++i) if (!std::isalnum(hdr[i])) alpha = false;
        if (alpha) {
            return (uint32_t(hdr[0])<<24)|(uint32_t(hdr[1])<<16)|(uint32_t(hdr[2])<<8)|uint32_t(hdr[3]);
        }
    }
    return 0;
}

extern "C" int SDL_main(int argc, char* argv[]) {
    auto filesDir = AndroidFilesDir::Get();
    if (!filesDir.empty()) {
        g_discGameCode = ReadDiscGameCode(filesDir / "rom");
        if (g_discGameCode != 0) {
            std::fprintf(stderr, "[android] disc game code %.4s (0x%08x)\n",
                reinterpret_cast<const char*>(&g_discGameCode), g_discGameCode);
        }
    }
    return RuntimeMain(argc, argv);
}

#endif // __ANDROID__
