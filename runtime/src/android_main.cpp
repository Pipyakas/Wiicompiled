#if defined(__ANDROID__)
#include <jni.h>
#include <filesystem>
#include <string>
#include <SDL3/SDL_main.h>

#include "android_files_dir.h"
#include "recomp_mod_loader.h"
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
    return static_cast<jint>(RecompMod::CurrentTranslatedExecutionAddress());
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
