#pragma once

#ifndef MKW_RUNTIME_LOG_H
#define MKW_RUNTIME_LOG_H

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#if defined(__ANDROID__)
#include <android/log.h>

// Line-buffered log helper: std::cerr is unbuffered (unitbuf), so each `<<`
// would otherwise flush as its own logcat line. The temporary lives until the
// end of the full `RT_LOG(...) << ... << std::endl;` statement, then logs the
// complete line in its destructor.
struct AndroidLogLine {
    std::ostringstream oss;
    ~AndroidLogLine() {
        std::string s = oss.str();
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t start = 0;
        bool any = false;
        while (start < s.size()) {
            size_t end = s.find('\n', start);
            std::string line = s.substr(start, end == std::string::npos ? end : end - start);
            if (!line.empty()) {
                __android_log_write(ANDROID_LOG_INFO, "WiiCompiled", line.c_str());
                any = true;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (!any) return;
        std::fprintf(stderr, "%s\n", s.c_str());
    }
    std::ostringstream& stream() { return oss; }
};
#endif

#include "memory.h"

// Canonical module tags. Use one of these; never write a bare "[...]" prefix
// into a message. (A message whose text spans several output lines repeats the
// tag inline on the continuation lines - the macros only prefix the first.)
#define RT_TAG_RUNTIME "runtime"
#define RT_TAG_CONFIG "runtime-config"
#define RT_TAG_MEMORY "memory"
#define RT_TAG_MOD "mod"
#define RT_TAG_HLE "hle"
#define RT_TAG_OS "os"
#define RT_TAG_GX "gx"
#define RT_TAG_AUDIO "audio"
#define RT_TAG_NET "net"
#define RT_TAG_DVD "dvd"
#define RT_TAG_NAND "nand"
#define RT_TAG_RIIVOLUTION "riivolution"
#define RT_TAG_VI "vi"

// Stream form:  RT_LOG(RT_TAG_OS) << "OSCreateThread failed" << std::endl;
#if defined(__ANDROID__)
#define RT_LOG(tag) (AndroidLogLine().stream() << "[" tag "] ")
#else
#define RT_LOG(tag) (std::cerr << "[" tag "] ")
#endif

// printf form:  RT_LOGF(RT_TAG_GX, "invalid GXTexObj @0x%08X\n", addr);
// `tag` and the format string must both be literals; they are concatenated.
#if defined(__ANDROID__)
#define RT_LOGF(tag, ...) do { __android_log_print(ANDROID_LOG_INFO, "WiiCompiled", "[" tag "] " __VA_ARGS__); std::fprintf(stderr, "[" tag "] " __VA_ARGS__); } while (0)
#else
#define RT_LOGF(tag, ...) std::fprintf(stderr, "[" tag "] " __VA_ARGS__)
#endif

// Shared epilogue for the `catch (const Memory::AccessViolation& e)` handlers
// spread across the HLE. `who` is the guest function or operation that faulted;
// the tag is its module.
inline void LogMemoryError(const char* tag, const char* who,
                           const ::Memory::AccessViolation& e)
{
    std::cerr << "[" << tag << "] " << who << ": memory error at 0x" << std::hex
              << e.address() << std::dec << " (" << e.reason() << ")" << std::endl;
}

#endif // MKW_RUNTIME_LOG_H
