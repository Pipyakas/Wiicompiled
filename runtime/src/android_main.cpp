#if defined(__ANDROID__)
#include <jni.h>
#include <filesystem>
#include <string>
#include <SDL3/SDL_main.h>

#include "android_files_dir.h"
#include "hle_stubs.h"
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
    char buf[512];
    std::snprintf(buf, sizeof(buf), "gxbeg=%llu gxend=%llu gxdl=%llu gxfifo=%llu dlbeg=%llu dlend=%llu dlact=%llu"
        " fdraw=%llu frawok=%llu frawfail=%llu fincr=%llu fnull=%llu funk=%llu fbp=%llu fcp=%llu fxf=%llu fib=%llu vrem=%llu fback=%llu nattr=%llu"
        " viadv=%llu vipost=%llu viguard=%llu viret=%llu",
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
        (unsigned long long)viguard, (unsigned long long)viret);
    // Temporary: one-shot guest thread dump per watchdog sample (logcat THRDUMP
    // lines). Remove with the GX counters.
    OS_HLE_DumpThreadsTemp();
    return env->NewStringUTF(buf);
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
