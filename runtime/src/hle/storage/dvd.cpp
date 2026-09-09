#include "hle_stubs.h"
#include "isa/big_endian.h"
#include "hle/dvd_contract.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"

// Defined in hle/gx/gx_objects.cpp. DVD reads are DMA-class writes to guest
// RAM: the game brackets them with DCInvalidateRange (not a flush), so the GX
// layer's caches must be notified explicitly that these bytes changed.
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);
#include "hle/storage/riivolution.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"

#if defined(__ANDROID__)
extern "C" {
#include <dolphin/dvd.h>
#include <aurora/dvd.h>
}
#endif
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

// ============================================================================
// Configuration
// ============================================================================

// The extracted ISO "DATA" folder. We map its "files" subfolder to the DVD
// root "/" and its "sys" subfolder to "/sys/". The root is user-owned input:
// it is never embedded into or copied by the public runtime.
static fs::path g_dvdRoot;
static std::once_flag g_dvdRootOnce;

static uint32_t CurrentDiscGameCode() {
#if defined(__ANDROID__)
    if (const uint32_t code = aurora_dvd_get_game_code();
        code != 0 && RuntimeHle::IsValidGameCode(code)) {
        return code;
    }
#endif
    return RuntimeHle::CurrentGameCode(0x524D4350u); // RMCP
}

// ============================================================================
// DVD Constants & Structures
// ============================================================================

#define DVD_STATE_FATAL_ERROR  -1
#define DVD_STATE_END           0
#define DVD_STATE_BUSY          1
#define DVD_STATE_WAITING       2
#define DVD_STATE_COVER_CLOSED  3
#define DVD_STATE_NO_DISK       4
#define DVD_STATE_COVER_OPEN    5

// Offsets in DVD Command Block (OS standard)
#define DVD_CB_OFFSET_STATE       0x0C
#define DVD_CB_OFFSET_TRANSFERRED 0x20
#define DVD_FILEINFO_OFFSET_ADDR  0x30
#define DVD_FILEINFO_OFFSET_LEN   0x34

struct DVDFileEntry {
    fs::path hostPath;
    std::string dvdPath;  // Virtual Wii path (e.g., "/Race/Course.szs")
    uint32_t size;
    uint32_t discOffsetWords = 0;
    bool isDirectory = false;
};

struct FstFileEntry {
    uint32_t start;
    uint32_t end;
    uint32_t size;
    std::string dvdPath;
    fs::path hostPath;
};

// Global State
static std::vector<DVDFileEntry> g_fileEntries;
static std::map<std::string, int32_t> g_pathToEntry;
// Maps each published file's disc range back to its runtime entry.
struct PublishedExtent {
    uint64_t startBytes = 0;
    uint64_t endBytes = 0;
    int32_t entryIndex = -1;
};
static std::vector<PublishedExtent> g_publishedExtents;
static bool g_dvdInitialized = false;
#if defined(__ANDROID__)
static bool g_dvdUseNod = false;
#endif
static std::vector<FstFileEntry> g_fstFiles;
static bool g_fstLoaded = false;
#if defined(__ANDROID__)
static std::map<uint32_t,int32_t> g_nodOffsetToEntry; // disc offset words -> nod FST index
#endif
static std::unordered_set<std::string> g_loggedReadErrors;
static constexpr int32_t kDvdFatalError = -3;

extern "C" void DVDInit_8015EA1C();

// This MEM2 region is reserved at startup for the published FST.
extern "C" uint32_t g_dvdFstReservedBase;
extern "C" uint32_t g_dvdFstReservedSize;

// Byte-wise copy into guest RAM plus the DMA notification the GX caches need.
static void CopyToGuestAsDma(uint32_t dest, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        Memory::Write8(dest + static_cast<uint32_t>(i), data[i]);
    }
    GxNotifyGuestRamDmaWrite(dest, static_cast<uint32_t>(size));
}

// Host path strings only ever leave this module as UTF-8 display text.
static std::string HostPathText(const fs::path& path) {
    return RuntimeConfigFile::PathToUtf8(path);
}

static bool IsDvdDataRoot(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec &&
           fs::is_directory(path / "files", ec) && !ec &&
           fs::is_regular_file(path / "sys" / "fst.bin", ec) && !ec;
}

// Single fatal idiom for this module: crash artifacts under `reason`, the same
// text in the popup, a non-zero exit code, and no second generic report from
// the atexit handler.
[[noreturn]] static void FailDvd(const char* reason, const char* category,
                                 const std::string& details) {
    RuntimeCrash::WriteCrashArtifacts(reason, details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup(category, details);
    MarkFatalErrorReported();
#if defined(__ANDROID__)
    std::_Exit(EXIT_FAILURE);
#else
    std::exit(EXIT_FAILURE);
#endif
}

[[noreturn]] static void FailDvdRoot(const char* source, const fs::path& path = {}) {
    RT_LOGF(RT_TAG_DVD, "ERROR: %s", source);
    if (!path.empty()) {
        std::fprintf(stderr, ": %s", HostPathText(path).c_str());
    }
    std::fprintf(stderr,
                 "\n[dvd] Set [paths] dvd_root in Config.toml "
                 "to your extracted Mario Kart Wii DATA directory.\n");
    std::string details = source ? source : "The configured DVD root could not be opened.";
    if (!path.empty()) {
        details += "\n\nPath: ";
        details += HostPathText(path);
    }
    details += "\n\nSet [paths] dvd_root in Config.toml to the extracted "
               "Mario Kart Wii DATA directory.";
    FailDvd("dvd_root", "DVD data is unavailable", details);
}

static const fs::path& GetDvdRoot() {
    std::call_once(g_dvdRootOnce, []() {
#if defined(__ANDROID__)
        // Android: Picker stages content under getFilesDir()/rom (which is also
        // what ExecutableDirectory() returns on Android). Accept an extracted
        // DATA dir or a raw image directory as the DVD root so we never
        // std::exit() while audio/mix threads still hold mutexes (which
        // surfaces as FORTIFY destroyed-mutex).
        if (auto exe = RuntimeConfigFile::ExecutableDirectory(); exe && !exe->empty()) {
            if (IsDvdDataRoot(*exe)) { g_dvdRoot = *exe; return; }
            if (IsDvdDataRoot(*exe / "rom")) { g_dvdRoot = *exe / "rom"; return; }
            std::error_code ec;
            if (fs::is_directory(*exe / "rom", ec) && !ec) { g_dvdRoot = *exe / "rom"; return; }
        }
#endif
        const fs::path path = RuntimeConfigFile::ResolvedDvdRoot();
        if (path.empty()) {
            FailDvdRoot("No DVD root is configured");
        }
        if (!IsDvdDataRoot(path)) {
#if defined(__ANDROID__)
            std::error_code ec;
            if (fs::is_directory(path, ec) && !ec) { g_dvdRoot = path; return; }
#endif
            FailDvdRoot("Configured DVD root is not an extracted DATA directory", path);
        }
        g_dvdRoot = path;
    });

    return g_dvdRoot;
}

static std::string NormalizePath(const std::string& path);
static fs::path ResolveDvdMappedHostPath(const std::string& dvdPath, const fs::path& fallbackHostPath);

// The DVD HLE fires translated callbacks synchronously, in the caller's
// thread, exactly like the SDK's synchronous-completion path: the callback
// shares the caller's register file, returns into it, and only r3 (the
// result) is defined afterwards. Running it on a copy would strand the
// state machine's own stores (r30/r31 updates, queue writes) in the copy.
// One-shot diagnostic: log which translated callback runs with which status
// so a stuck cover-wait shows whether its completion ever fires.
static void InvokeDvdCallback(uint32_t callbackPtr, int32_t result, uint32_t fileInfoPtr) {
    if (callbackPtr == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = static_cast<uint32_t>(result);
    cpu.gpr[4] = fileInfoPtr;
    InvokeIndirectCpu(callbackPtr, &cpu);
}

// Same shape as InvokeDvdCallback minus the command block: the DVDLow callbacks
// take only a result, so r4 is deliberately left untouched here.
static void InvokeDvdLowCallback(uint32_t callbackPtr, int32_t result) {
    if (callbackPtr == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = static_cast<uint32_t>(result);
    InvokeIndirectCpu(callbackPtr, &cpu);
}

static void ReportDvdReadError(const std::string& hostPath,
                               int64_t offset,
                               uint32_t length,
                               const char* reason) {
    const std::string key = hostPath + "\n" + std::to_string(offset);
    if (!g_loggedReadErrors.insert(key).second) {
        return;
    }

    RT_LOGF(RT_TAG_DVD,
                 "ERROR: DVD read failed: host=\"%s\" offset=%lld (0x%llx) "
                 "length=%u: %s\n",
                 hostPath.c_str(),
                 static_cast<long long>(offset),
                 static_cast<unsigned long long>(offset),
                 length,
                 reason ? reason : "unknown read error");
}

static int32_t DvdReadFatal(uint32_t fileInfoPtr,
                            const std::string& hostPath,
                            int64_t offset,
                            uint32_t length,
                            const char* reason) {
    ReportDvdReadError(hostPath, offset, length, reason);
    try {
        Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_STATE,
                        static_cast<uint32_t>(DVD_STATE_FATAL_ERROR));
        Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_TRANSFERRED, 0);
    } catch (const Memory::AccessViolation&) {
    }
    return kDvdFatalError;
}

static void LoadFstIndex() {
    if (g_fstLoaded) {
        return;
    }
    g_fstLoaded = true;

    const fs::path fstPath = GetDvdRoot() / "sys" / "fst.bin";
    std::ifstream fstFile(fstPath, std::ios::binary);
    if (!fstFile.is_open()) {
        return;
    }

    fstFile.seekg(0, std::ios::end);
    const std::streamsize size = fstFile.tellg();
    fstFile.seekg(0, std::ios::beg);
    if (size < 12) {
        return;
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    fstFile.read(reinterpret_cast<char*>(data.data()), size);
    if (!fstFile) {
        return;
    }

    const uint32_t entryCount = BigEndian::Read32(&data[8]);
    if (entryCount == 0 || entryCount > 0x10000) {
        return;
    }

    const size_t entriesSize = static_cast<size_t>(entryCount) * 12;
    if (entriesSize >= data.size()) {
        return;
    }

    const size_t stringBase = entriesSize;

    struct DirFrame {
        uint32_t endIndex;
        std::string path;
    };

    std::vector<DirFrame> stack;
    stack.push_back({entryCount, std::string()});

    g_fstFiles.clear();
    g_fstFiles.reserve(entryCount / 2);

    for (uint32_t i = 1; i < entryCount; ++i) {
        while (!stack.empty() && i >= stack.back().endIndex) {
            stack.pop_back();
        }
        if (stack.empty()) {
            break;
        }

        const size_t entryOff = static_cast<size_t>(i) * 12;
        const uint32_t nameWord = BigEndian::Read32(&data[entryOff]);
        const uint8_t type = static_cast<uint8_t>(nameWord >> 24);
        const uint32_t nameOffset = nameWord & 0x00FFFFFFu;
        if (stringBase + nameOffset >= data.size()) {
            break;
        }

        const char* namePtr = reinterpret_cast<const char*>(&data[stringBase + nameOffset]);
        std::string name(namePtr);

        if (type != 0) {
            const uint32_t nextIndex = BigEndian::Read32(&data[entryOff + 8]);
            std::string dirPath = stack.back().path;
            if (!dirPath.empty()) {
                dirPath += "/";
            }
            dirPath += name;
            stack.push_back({nextIndex, dirPath});
            continue;
        }

        const uint32_t fileOffset = BigEndian::Read32(&data[entryOff + 4]);
        const uint32_t fileSize = BigEndian::Read32(&data[entryOff + 8]);

        std::string relPath = stack.back().path;
        if (!relPath.empty()) {
            relPath += "/";
        }
        relPath += name;

        const uint64_t startBytes64 = static_cast<uint64_t>(fileOffset) * 4ull;
        if (startBytes64 > 0xFFFFFFFFu) {
            continue;
        }
        const uint32_t startBytes = static_cast<uint32_t>(startBytes64);
        const uint32_t endBytes = startBytes + fileSize;

        FstFileEntry entry;
        entry.start = startBytes;
        entry.end = endBytes;
        entry.size = fileSize;
        entry.dvdPath = "/" + relPath;
        const fs::path baseHostPath = GetDvdRoot() / "files" / fs::path(relPath);
        entry.hostPath = ResolveDvdMappedHostPath(entry.dvdPath, baseHostPath);
        g_fstFiles.push_back(std::move(entry));
    }

    std::sort(g_fstFiles.begin(), g_fstFiles.end(),
              [](const FstFileEntry& a, const FstFileEntry& b) { return a.start < b.start; });

}

static const PublishedExtent* FindPublishedExtentForByteOffset(uint64_t offset) {
    if (g_publishedExtents.empty()) {
        return nullptr;
    }

    auto it = std::upper_bound(
        g_publishedExtents.begin(), g_publishedExtents.end(), offset,
        [](uint64_t value, const PublishedExtent& extent) { return value < extent.startBytes; });
    if (it == g_publishedExtents.begin()) {
        return nullptr;
    }
    --it;
    if (offset >= it->startBytes && offset < it->endBytes) {
        return &(*it);
    }
    return nullptr;
}

struct AbsReadResult {
    const DVDFileEntry* entry = nullptr;
    uint32_t fileOffset = 0;
    uint32_t readLength = 0;
};

static bool ResolveAbsRead(uint32_t offset, uint32_t length, AbsReadResult& out) {
    // The extent table is available after DVDInit publishes the FST.
    DVDInit_8015EA1C();
    const PublishedExtent* extent = FindPublishedExtentForByteOffset(offset);
    uint64_t effectiveOffset = offset;

    if (!extent) {
        // Accept both byte offsets and the FST's four-byte word offsets.
        const uint64_t scaledOffset = static_cast<uint64_t>(offset) * 4ull;
        extent = FindPublishedExtentForByteOffset(scaledOffset);
        if (extent) {
            effectiveOffset = scaledOffset;
        }
    }

    if (!extent) {
        return false;
    }

    const DVDFileEntry& entry = g_fileEntries[extent->entryIndex];
    const uint32_t fileOffset = static_cast<uint32_t>(effectiveOffset - extent->startBytes);
    uint32_t readLength = length;
    if (readLength == 0 || fileOffset + readLength > entry.size) {
        readLength = entry.size - fileOffset;
    }

    out.entry = &entry;
    out.fileOffset = fileOffset;
    out.readLength = readLength;
    return true;
}

// Helper: Normalize paths (Windows backslash -> forward slash, lowercase for lookup)
static std::string NormalizePath(const std::string& path) {
    return DvdFstContract::NormalizeLookupPath(path);
}

static void RegisterFileEntry(std::string dvdPath, const fs::path& hostPath, uint32_t size) {
    dvdPath = DvdFstContract::CanonicalizePath(dvdPath);

    DVDFileEntry fileEntry;
    fileEntry.hostPath = hostPath;
    fileEntry.dvdPath = dvdPath;
    fileEntry.size = size;

    const int32_t entryNum = static_cast<int32_t>(g_fileEntries.size());
    g_fileEntries.push_back(std::move(fileEntry));
    g_pathToEntry[NormalizePath(dvdPath)] = entryNum;
}

// True when a disc path already has an entry (vanilla disc or an earlier
// overlay registration). Uses the same canonicalization as RegisterFileEntry.
static bool DvdEntryExists(const std::string& dvdPath) {
    return g_pathToEntry.find(NormalizePath(DvdFstContract::CanonicalizePath(dvdPath))) !=
           g_pathToEntry.end();
}

// Walk `root` and hand every entry to `visit`. Uses the error_code iterator forms so an
// unreadable entry doesn't abort the rest of the overlay. `announceErrors` keeps the
// disc-index scan noisy and the by-name mapping quiet, matching each caller's old behavior.
template <typename Visit>
static void WalkDirectory(const fs::path& root, bool recursive, bool announceErrors, Visit&& visit) {
    std::error_code ec;

    if (!recursive) {
        for (const auto& entry : fs::directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            visit(entry);
        }
        return;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (announceErrors) {
            RT_LOG(RT_TAG_DVD) << "WARNING: cannot enumerate " << HostPathText(root) << ": "
                      << ec.message() << std::endl;
        }
        return;
    }

    const fs::recursive_directory_iterator end;
    while (it != end) {
        const fs::directory_entry entry = *it;

        it.increment(ec);
        if (ec) {
            if (announceErrors) {
                RT_LOG(RT_TAG_DVD) << "WARNING: stopped enumerating " << HostPathText(root) << ": "
                          << ec.message() << std::endl;
                return;
            }
            visit(entry);
            return;
        }

        visit(entry);
    }
}

// Helper: Scan directory and build file index.
// addNewFiles=false is Riivolution's create="false": only files already on
// the disc may be replaced, nothing new is added.
static void ScanDirectory(const fs::path& root, const std::string& virtualPrefix,
                          bool recursive = true, bool addNewFiles = true) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }

    const auto registerEntry = [&](const fs::directory_entry& entry) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            return;
        }

        const std::uintmax_t size = fs::file_size(entry.path(), entryEc);
        if (entryEc) {
            RT_LOG(RT_TAG_DVD) << "WARNING: skipping " << HostPathText(entry.path()) << ": "
                      << entryEc.message() << std::endl;
            return;
        }

        const fs::path relative = fs::relative(entry.path(), root, entryEc);
        if (entryEc) {
            return;
        }

        std::string prefix = virtualPrefix.empty() ? "/" : virtualPrefix;
        if (prefix.back() != '/' && prefix.back() != '\\') {
            prefix += "/";
        }
        std::string dvdPath = prefix + HostPathText(relative);
        if (!addNewFiles && !DvdEntryExists(dvdPath)) {
            return;
        }
        RegisterFileEntry(std::move(dvdPath), entry.path(), static_cast<uint32_t>(size));
    };

    WalkDirectory(root, recursive, /*announceErrors=*/true, registerEntry);
}

// Riivolution <folder> without a disc path: replace disc files that share a
// filename with a file in the external folder (Dolphin's
// FindFilenameNodeInFST behavior, applied across the whole index).
static void ApplyFolderByNameMapping(const RuntimeRiivolution::Mapping& mapping) {
    // Snapshot the current index: matches by filename may only target entries
    // that exist before this mapping, and RegisterFileEntry appends.
    std::map<std::string, std::vector<std::string>> discPathsByName;
    const size_t entryCount = g_fileEntries.size();
    for (size_t i = 0; i < entryCount; ++i) {
        const std::string& dvdPath = g_fileEntries[i].dvdPath;
        const size_t lastSlash = dvdPath.rfind('/');
        std::string name = lastSlash == std::string::npos ? dvdPath : dvdPath.substr(lastSlash + 1);
        RuntimeHle::LowerInPlace(name);
        if (!name.empty()) {
            discPathsByName[name].push_back(dvdPath);
        }
    }

    uint32_t replaced = 0;
    const auto applyEntry = [&](const fs::directory_entry& entry) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            return;
        }
        std::string name = HostPathText(entry.path().filename());
        RuntimeHle::LowerInPlace(name);
        const auto matches = discPathsByName.find(name);
        if (matches == discPathsByName.end()) {
            return;
        }
        const std::uintmax_t size = fs::file_size(entry.path(), entryEc);
        if (entryEc) {
            return;
        }
        for (const std::string& dvdPath : matches->second) {
            RegisterFileEntry(dvdPath, entry.path(), static_cast<uint32_t>(size));
            ++replaced;
        }
    };

    WalkDirectory(mapping.hostPath, mapping.recursive, /*announceErrors=*/false, applyEntry);

    RT_LOG(RT_TAG_DVD) << HostPathText(mapping.hostPath) << ": replaced " << replaced
              << " disc file(s) by filename" << std::endl;
}

static void ScanOverlayRoot(const RuntimeRiivolution::Overlay& overlay) {
    if (!overlay.patches) {
        // Fallback for mod roots that mirror the disc filesystem directly (not a
        // Riivolution pack, which wouldn't map anything useful this way).
        RT_LOG(RT_TAG_DVD) << HostPathText(overlay.root)
                  << ": no Riivolution XML found, treating the root as a disc-shaped overlay"
                  << std::endl;
        ScanDirectory(overlay.root, "/");
        return;
    }

    for (const auto& mapping : overlay.patches->mappings) {
        switch (mapping.kind) {
        case RuntimeRiivolution::Mapping::Kind::File: {
            if (!mapping.create && !DvdEntryExists(mapping.discPath)) {
                break;
            }
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(mapping.hostPath, ec);
            if (!ec) {
                RegisterFileEntry(mapping.discPath, mapping.hostPath, static_cast<uint32_t>(size));
            }
            break;
        }
        case RuntimeRiivolution::Mapping::Kind::Folder:
            ScanDirectory(mapping.hostPath, mapping.discPath, mapping.recursive, mapping.create);
            break;
        case RuntimeRiivolution::Mapping::Kind::FolderByName:
            ApplyFolderByNameMapping(mapping);
            break;
        }
    }
}

static fs::path ResolveDvdMappedHostPath(const std::string& dvdPath, const fs::path& fallbackHostPath) {
    const std::string normalized = NormalizePath(dvdPath);
    const auto it = g_pathToEntry.find(normalized);
    if (it != g_pathToEntry.end() && it->second >= 0 &&
        it->second < static_cast<int32_t>(g_fileEntries.size())) {
        return g_fileEntries[it->second].hostPath;
    }

    for (const auto& overlay : RuntimeRiivolution::Overlays()) {
        const fs::path candidate = overlay.root / fs::path(normalized.substr(1));
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    return fallbackHostPath;
}

[[noreturn]] static void FailRuntimeFst(const std::string& reason) {
    RT_LOGF(RT_TAG_DVD, "ERROR: cannot publish the runtime FST: %s\n", reason.c_str());
    FailDvd("dvd_fst", "DVD file table is unavailable", reason);
}

static void BuildAndPublishRuntimeFst() {
    // Preserve real disc offsets where a runtime entry replaces a file from the
    // extracted image. Created Riivolution paths have no physical disc extent;
    // they are opened by their shared FST/SDK entry number instead.
    for (const FstFileEntry& fstFile : g_fstFiles) {
        const auto mapped = g_pathToEntry.find(NormalizePath(fstFile.dvdPath));
        if (mapped == g_pathToEntry.end() || mapped->second < 0 ||
            mapped->second >= static_cast<int32_t>(g_fileEntries.size())) {
            continue;
        }
        g_fileEntries[mapped->second].discOffsetWords = fstFile.start / 4u;
    }

    // Give runtime-added files unique synthetic disc ranges so reads can find them.
    uint64_t nextFreeBytes = 0;
    for (const FstFileEntry& fstFile : g_fstFiles) {
        nextFreeBytes = std::max(nextFreeBytes, static_cast<uint64_t>(fstFile.end));
    }
    for (const DVDFileEntry& entry : g_fileEntries) {
        if (entry.discOffsetWords != 0) {
            nextFreeBytes = std::max(
                nextFreeBytes, static_cast<uint64_t>(entry.discOffsetWords) * UINT64_C(4) +
                                   static_cast<uint64_t>(entry.size));
        }
    }
    for (DVDFileEntry& entry : g_fileEntries) {
        if (entry.isDirectory || entry.discOffsetWords != 0) {
            continue;
        }
        nextFreeBytes = (nextFreeBytes + 31ull) & ~31ull;
        if (nextFreeBytes / 4ull > 0xFFFFFFFFull) {
            FailRuntimeFst("synthetic disc extents exceed the 32-bit FST word offset range");
        }
        entry.discOffsetWords = static_cast<uint32_t>(nextFreeBytes / 4ull);
        nextFreeBytes += std::max<uint32_t>(entry.size, 4u);
    }

    std::vector<DvdFstContract::RegisteredFile> registrations;
    registrations.reserve(g_fileEntries.size());
    for (const DVDFileEntry& entry : g_fileEntries) {
        registrations.push_back({entry.hostPath, entry.dvdPath, entry.size, entry.discOffsetWords});
    }

    DvdFstContract::Image image;
    try {
        image = DvdFstContract::BuildImage(registrations);
    } catch (const std::exception& error) {
        FailRuntimeFst(error.what());
    }

    std::vector<DVDFileEntry> indexedEntries;
    indexedEntries.reserve(image.entries.size());
    for (const DvdFstContract::IndexedEntry& entry : image.entries) {
        indexedEntries.push_back({entry.hostPath, entry.dvdPath, entry.size,
                                  entry.discOffsetWords, entry.isDirectory});
    }
    g_fileEntries = std::move(indexedEntries);
    g_pathToEntry = std::move(image.pathToEntry);

    g_publishedExtents.clear();
    g_publishedExtents.reserve(g_fileEntries.size());
    for (size_t i = 0; i < g_fileEntries.size(); ++i) {
        const DVDFileEntry& entry = g_fileEntries[i];
        if (entry.isDirectory || entry.discOffsetWords == 0) {
            continue;
        }
        const uint64_t startBytes = static_cast<uint64_t>(entry.discOffsetWords) * 4ull;
        // Give empty files a one-byte range so their handles still resolve.
        const uint64_t endBytes = startBytes + std::max<uint32_t>(entry.size, 1u);
        g_publishedExtents.push_back({startBytes, endBytes, static_cast<int32_t>(i)});
    }
    std::sort(g_publishedExtents.begin(), g_publishedExtents.end(),
              [](const PublishedExtent& a, const PublishedExtent& b) {
                  return a.startBytes < b.startBytes;
              });

    // Use the FST memory reserved before guest code can allocate over it.
    if (g_dvdFstReservedBase == 0 || image.bytes.size() > g_dvdFstReservedSize ||
        !Memory::Contains(g_dvdFstReservedBase, g_dvdFstReservedSize)) {
        std::ostringstream reason;
        reason << "The " << image.bytes.size() << "-byte runtime FST does not fit the "
               << g_dvdFstReservedSize << "-byte guest reservation at 0x" << std::hex
               << g_dvdFstReservedBase << ".";
        FailRuntimeFst(reason.str());
    }

    const uint32_t fstAddress = g_dvdFstReservedBase;
    for (size_t i = 0; i < image.bytes.size(); ++i) {
        Memory::Write8(fstAddress + static_cast<uint32_t>(i), image.bytes[i]);
    }

    Memory::Write32(0x80000038u, fstAddress);
    Memory::Write32(0x8000003Cu, static_cast<uint32_t>(image.bytes.size()));

    RT_LOG(RT_TAG_DVD) << "published " << image.entries.size() << " FST entries ("
              << image.bytes.size() << " bytes) at 0x" << std::hex << fstAddress
              << " inside the boot-time MEM2 reservation" << std::dec << std::endl;

    if (const char* dumpPath = std::getenv("MKW_DUMP_FST")) {
        std::ofstream dump(dumpPath, std::ios::binary);
        dump.write(reinterpret_cast<const char*>(image.bytes.data()),
                   static_cast<std::streamsize>(image.bytes.size()));
        RT_LOG(RT_TAG_DVD) << "dumped published FST to " << dumpPath << std::endl;
    }
}

extern "C" const char* DVDResolveHostPathForTest(const char* dvdPath)
{
    static std::string resolved;
    resolved.clear();

    if (!dvdPath || dvdPath[0] == '\0') {
        return nullptr;
    }

    DVDInit_8015EA1C();
    const std::string normalized = NormalizePath(dvdPath);
    const auto it = g_pathToEntry.find(normalized);
    if (it == g_pathToEntry.end() ||
        it->second < 0 ||
        it->second >= static_cast<int32_t>(g_fileEntries.size()) ||
        g_fileEntries[it->second].isDirectory) {
        return nullptr;
    }

    resolved = HostPathText(g_fileEntries[it->second].hostPath);
    return resolved.c_str();
}

// ============================================================================
// High-Level DVD API
// ============================================================================

static void InitDvdWaitingQueues()
{
    constexpr uint32_t kQueueBase = 0x80343230;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t queue = kQueueBase + (i * 8);
        Memory::Write32(queue + 0, queue);
        Memory::Write32(queue + 4, queue);
    }
}

static void CompleteDvdCancelState()
{
    InitDvdWaitingQueues();

    // These are the SDK DVD globals used by DVDCancelAll/__DVDPrepareReset.
    // The actual drive work is HLE'd, so complete pending cancel/reset waits.
    Memory::Write32(0x80386664, 0); // Canceling
    Memory::Write32(0x80386668, 0); // ResumeFromHere
    Memory::Write32(0x80386670, 0); // PausingFlag
    Memory::Write32(0x8038667C, 1); // CancelAllSync complete
    Memory::Write32(0x803866A8, 1); // PrepareReset complete
    Memory::Write32(0x803866F0, 0); // executing command block
}

// Try opening a compressed disc image (RVZ/WIA/WBFS/CISO) via aurora_dvd (nod)
// before falling back to the extracted DATA scan.  On Android the image
// lives in getFilesDir()/rom/ and is region/format agnostic.
#if defined(__ANDROID__)
extern "C" bool aurora_dvd_open(const char* path);
extern "C" bool aurora_dvd_get_raw_fst(const uint8_t** out_data, size_t* out_size);
static bool TryOpenCompressedDisc() {
    std::error_code ec;
    std::vector<fs::path> candidates;
    auto collect = [&](const fs::path& dir) {
        std::error_code ec2;
        if (!fs::is_directory(dir, ec2) || ec2) return;
        for (auto& e : fs::directory_iterator(dir, ec2)) {
            if (ec2) break;
            if (!e.is_regular_file(ec2) || ec2) continue;
            auto ext = e.path().extension().string();
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext==".rvz"||ext==".wia"||ext==".wbfs"||ext==".iso"||ext==".ciso"||ext==".wdf"||ext==".gcm")
                candidates.push_back(e.path());
        }
    };
    // ExecutableDirectory() returns the app-private files dir on Android.
    if (auto exe = RuntimeConfigFile::ExecutableDirectory(); exe && !exe->empty())
        collect(*exe / "rom");
    else if (const char* env = std::getenv("WIICOMPILED_FILES_DIR"); env && *env)
        collect(fs::path(env) / "rom");
    // Also try the configured dvd_root if it points at a file
    {
        auto cfg = RuntimeConfigFile::ResolvedDvdRoot();
        if (!cfg.empty()) {
            std::error_code ec3;
            if (fs::is_regular_file(cfg, ec3) && !ec3) candidates.push_back(cfg);
            else collect(cfg);
        }
    }
    // Prefer the largest image (main game) when multiple roms are staged
    std::sort(candidates.begin(), candidates.end(), [](const fs::path& a, const fs::path& b){
        std::error_code ea, eb;
        auto sa = fs::file_size(a, ea); auto sb = fs::file_size(b, eb);
        if (ea || eb) return a.string() < b.string();
        return sa > sb;
    });
    for (auto& cand : candidates) {
        std::string s = RuntimeConfigFile::PathToUtf8(cand);
        RT_LOG(RT_TAG_DVD) << "trying compressed disc: " << s << std::endl;
        if (aurora_dvd_open(s.c_str())) {
            RT_LOG(RT_TAG_DVD) << "opened compressed disc via nod: " << s << std::endl;
            g_dvdUseNod = true;
            return true;
        }
    }
    return false;
}
#endif

// 0x8015EA1C -> DVDInit
extern "C" void DVDInit_8015EA1C()
{
    if (g_dvdInitialized) return;
    // The scan below builds g_fileEntries incrementally, so a re-entrant call
    // must not start a second scan on top of a half-built index and duplicate
    // every entry.
    static bool initializing = false;
    if (initializing) return;
    initializing = true;


    // 1. Initialize Global Flags (Emulate OS state)
    // These addresses are standard OS globals for DVD context
    Memory::Write8(0x80386724, 1);   // Contexts initialized
    Memory::Write8(0x80386725, 1);   // LowInit called
    Memory::Write32(0x80386720, 0);  // Current context index
    Memory::Write8(0x803866a0, 1);   // DVDInit called flag
    CompleteDvdCancelState();
    // NOTE: the hardware drive queue at 0x80343230/38/40/48 is left exactly
    // as InitDvdWaitingQueues seeds it (self-linked = empty). Zeroing those
    // slots corrupts the translated dequeue path: func_80163660 follows the
    // "head" into address 0 and writes there. Cover/status words only:
    // -26016 -> 0x80386660: command counter, incremented per issued command
    // at func_8016193C entry (loc_80161984..801619A8) and capped at 5.
    // Keep at 0 for the first command.
    // -26008 -> 0x80386668, -26004 -> 0x8038666C: cover-wait gates in
    // func_80162B50. Nonzero -26004 returns -1, which the cover thread
    // (func_80008D18) maps to status 5 = done, exiting the loop. Seed
    // drive-ready (cover closed) so boot proceeds past cover-wait.
    Memory::Write32(0x80386660u, 0);
    Memory::Write32(0x80386668u, 0);
    Memory::Write32(0x8038666Cu, 1);
    // -25872 -> 0x80386730 must differ from the queue-block address the
    // guest compares it against, or func_80162A88 takes the not-ready path.
    // The SDK's own reset value is the queue base, so write that.
    Memory::Write32(0x80386730u, 0x80343230u);
    // Cover-register block for stateCoverClosed_CMD (func_801664E0): r7 =
    // 0x80343550, r6 = r7 + ((-25824) rotl 5 & -32); the register byte at
    // (r6 + 8) must be nonzero or the CMD spins at loc_8016654C. r9 =
    // -25824/0x80386760: seed 0 so the rotation offset is 0 and r6 = r7.
    Memory::Write32(0x80386760u, 0);
    Memory::Write32(0x80343550u, 0);
    Memory::Write8(0x80343558u, 1);
    // Cover register r6+12 (0x8034355C): func_80165A30 (loc_80165AA0),
    // func_80166678 (loc_80166710), and func_8016473C (loc_80164778) check
    // (*(r6+12) + 0x1150000) == 56045 (0xDAED) and spin forever otherwise.
    // r6+12 aliases the DI status word the status read writes (outBuf[0]);
    // the value observed in the log probe is 0 there before the first
    // transfer completes. Leave it zeroed: the setup gate
    // (func_80166678 loc_80166710) compares against the transferred status
    // once IOS completes, not against this seed.
    Memory::Write32(0x8034355Cu, 0);
    // Inquiry-completion state consumed by the translated callback chain
    // (func_80161EEC). After the callback entry, r0=0 at loc_80162058, so it
    // reaches loc_80162064 which compares -25904/0x803866D0 against 2: equal
    // EXITS at loc_801628B0 (drive considered already handled), NOT equal
    // falls to the ReadDiskID re-issue at loc_80162070. Write 0 there so the
    // state machine advances. The branch at loc_80162168 checks
    // -25980/0x8038667C (0 = skip the cancel path to loc_801621D4); the
    // loc_801621D4 test is (r31 & 1) on the DI status we pass (0x1 has bit 0
    // set, so it advances, without the bit-1 re-issue). The callback entry
    // gate (loc_80161F90/80161F9C) requires drive state 3 or 15 to take the
    // completion path at all — anything else diverts to the loc_801620EC
    // error path. Seed 3, matching the 0x1 advance-only status.
    Memory::Write32(0x80386678u, 0);
    Memory::Write32(0x80386670u, 0);
    Memory::Write32(0x803866E4u, 3);
    Memory::Write32(0x80386674u, 0);
    Memory::Write32(0x803866D0u, 0);
    Memory::Write32(0x80386688u, 0);
    Memory::Write32(0x8038668Cu, 0);
    // -25980 -> 0x8038667C: CancelAllSync flag. The translated callback
    // writes 10 there at loc_80162174 (and 0 at loc_80162172 for the
    // re-issue case); seed 0 = no cancel in progress.
    Memory::Write32(0x8038667Cu, 0);
    // -25900 -> 0x803866D4: set to 1 by the callback at loc_80161FDC when
    // the drive state reads 15 (cover-closed/ready). Pre-seed 1 so the
    // loc_801620EC error path reads ready even before the first callback.
    Memory::Write32(0x803866D4u, 1);
    // -25888 -> 0x803866E0: callback slot read at loc_801621B0/801621CC.
    // Zero = no pending async completion; the callback then re-drives the
    // state via func_80161614 at loc_801621CC instead of dispatching a
    // stale pointer through ctr.
    Memory::Write32(0x803866E0u, 0);
    // -29476 from r13 (0x8038CC00 - 29476 = 0x8037F914): command-type value
    // read at loc_80162120. The loc_80162114 fallthrough compares *(r29+8)
    // against it; the *(r29+8) it compares is the Inquiry command type 14,
    // so seed 14 to match and take the ready path (r0=1) instead of the
    // error path (r0=0 -> cancel branch). Do NOT seed the drive-state value
    // here: with a cover-status-looking type the (r31 & 1) advance test at
    // loc_801621D4 would need bit0 set that cmd122's status may not carry.
    Memory::Write32(0x8038CC00u - 29476u, 14);
    // -29464 from r13 (0x8038CC00 - 29464 = 0x8037F920): /dev/di fd written
    // by the translated DVDLowInit open and read as the fd for every DI
    // transfer the periodic re-issue path issues via IOS_IoctlAsync
    // (func_80165A30 / func_80166678). DVDLowInit is HLE'd so the
    // translated open never runs; seed our stable /dev/di fd (5) directly
    // or those transfers go out on fd 0 and fail.
    Memory::Write32(0x8038CC00u - 29464u, 5);

    // 2. Initialize DVD Context structures (prevent crashes in callbacks)
    constexpr uint32_t kContextBase = 0x803434e0;
    constexpr uint32_t kMagicValue = 0xFEEBDAED;
    for (int i = 0; i < 4; i++) {
        uint32_t ctx = kContextBase + i * 0x20;
        Memory::Write32(ctx + 0x0C, kMagicValue);
        Memory::Write32(ctx + 0x10, i);
    }

    // 3. Set Low Memory Globals (The "Magic" Identification)
    // This tells the game "Yes, I am Mario Kart Wii"
    uint32_t diskHeader = 0x80000000;
    Memory::Write32(diskHeader + 0x00, CurrentDiscGameCode());
    Memory::Write16(diskHeader + 0x04, 0x3031);     // '01' (Maker)
    Memory::Write8(diskHeader + 0x06, 0x01);        // Disk #1
#if defined(__ANDROID__)
    if (TryOpenCompressedDisc()) {
        bool usedRawFst = false;
        {
            const uint8_t* raw = nullptr; size_t rawSize = 0;
            if (aurora_dvd_get_raw_fst(&raw, &rawSize) && raw && rawSize >= 12) {
                const uint8_t* d = raw;
                uint32_t entryCount = (uint32_t(d[8])<<24)|(uint32_t(d[9])<<16)|(uint32_t(d[10])<<8)|uint32_t(d[11]);
                if (entryCount > 0 && entryCount <= 0x10000) {
                    size_t entriesSize = size_t(entryCount)*12;
                    if (entriesSize < rawSize) {
                        size_t stringBase = entriesSize;
                        struct DirFrame { uint32_t endIndex; std::string path; };
                        std::vector<DirFrame> stack; stack.push_back({entryCount, ""});
                        g_fstFiles.clear(); g_fstFiles.reserve(entryCount/2);
                        for (uint32_t i=1;i<entryCount;++i) {
                            while(!stack.empty() && i >= stack.back().endIndex) stack.pop_back();
                            if(stack.empty()) break;
                            size_t off = size_t(i)*12;
                            uint32_t nameWord = (uint32_t(d[off])<<24)|(uint32_t(d[off+1])<<16)|(uint32_t(d[off+2])<<8)|uint32_t(d[off+3]);
                            uint8_t type = uint8_t(nameWord>>24);
                            uint32_t nameOff = nameWord & 0x00FFFFFFu;
                            if (stringBase+nameOff >= rawSize) break;
                            const char* nm = reinterpret_cast<const char*>(&d[stringBase+nameOff]);
                            std::string name(nm);
                            if (type!=0) {
                                uint32_t nextIndex = (uint32_t(d[off+8])<<24)|(uint32_t(d[off+9])<<16)|(uint32_t(d[off+10])<<8)|uint32_t(d[off+11]);
                                std::string dirPath = stack.back().path;
                                if(!dirPath.empty()) dirPath+="/";
                                dirPath+=name;
                                stack.push_back({nextIndex, dirPath});
                                continue;
                            }
                            uint32_t fileOffset = (uint32_t(d[off+4])<<24)|(uint32_t(d[off+5])<<16)|(uint32_t(d[off+6])<<8)|uint32_t(d[off+7]);
                            uint32_t fileSize   = (uint32_t(d[off+8])<<24)|(uint32_t(d[off+9])<<16)|(uint32_t(d[off+10])<<8)|uint32_t(d[off+11]);
                            std::string relPath = stack.back().path;
                            if(!relPath.empty()) relPath+="/";
                            relPath+=name;
                            uint64_t startBytes64 = uint64_t(fileOffset)*4ull;
                            if(startBytes64>0xFFFFFFFFu) continue;
                            uint32_t startBytes = uint32_t(startBytes64);
                            uint32_t endBytes = startBytes + fileSize;
                            FstFileEntry entry; entry.start=startBytes; entry.end=endBytes; entry.size=fileSize;
                            entry.dvdPath="/"+relPath; entry.hostPath=fs::path(entry.dvdPath);
                            g_nodOffsetToEntry[uint32_t(startBytes/4u)] = int32_t(i);
                            g_fstFiles.push_back(std::move(entry));
                        }
                        std::sort(g_fstFiles.begin(), g_fstFiles.end(), [](const FstFileEntry& a, const FstFileEntry& b){return a.start<b.start;});
                        RT_LOG(RT_TAG_DVD) << "using raw disc FST: " << g_fstFiles.size() << " file(s), " << rawSize << " bytes" << std::endl;
                        usedRawFst = true;
                    }
                }
            }
        }
        if (!usedRawFst) {
            auto collectFromNod = [&]() {
                DVDDir root;
                if (!DVDOpenDir("/", &root)) return;
                std::function<void(const std::string&)> walk = [&](const std::string& dirPath){
                    DVDDir dir;
                    s32 ent = DVDConvertPathToEntrynum(dirPath.c_str());
                    if (ent < 0 || !DVDFastOpenDir(ent, &dir)) return;
                    DVDDirEntry e;
                    while (DVDReadDir(&dir, &e)) {
                        std::string child = dirPath;
                        if (child.back() != '/') child += '/';
                        child += e.name ? e.name : "";
                        if (e.isDir) walk(child);
                        else { s32 fEnt=(int32_t)e.entryNum; DVDFileInfo fi{}; if(DVDFastOpen(fEnt,&fi)){ RegisterFileEntry(child, fs::path(child), fi.length); DVDClose(&fi); } }
                    }
                };
                walk("/");
                RT_LOG(RT_TAG_DVD) << "indexed " << g_fileEntries.size() << " file(s) from compressed disc (fallback enum)" << std::endl;
            };
            collectFromNod();
        } else {
            for (auto &fe : g_fstFiles) RegisterFileEntry(fe.dvdPath, fe.hostPath, fe.size);
            RT_LOG(RT_TAG_DVD) << "indexed " << g_fileEntries.size() << " file(s) from raw FST" << std::endl;
        }
        g_fstLoaded = true;
        const auto& overlays = RuntimeRiivolution::Overlays();
        const size_t vanillaEntryCount = g_fileEntries.size();
        for (auto overlay = overlays.rbegin(); overlay != overlays.rend(); ++overlay) ScanOverlayRoot(*overlay);
        RT_LOG(RT_TAG_DVD) << "disc index: " << vanillaEntryCount << " disc file(s), "
              << (g_fileEntries.size() - vanillaEntryCount) << " overlay registration(s) from "
              << overlays.size() << " root(s)" << std::endl;
        BuildAndPublishRuntimeFst();
        constexpr uint32_t kDvdFsInitAddress = 0x8015DF1C;
        if (TranslatedFunctionRegistry::FindByAddressPtr(kDvdFsInitAddress)) InvokeIndirectCpu(kDvdFsInitAddress, &GetPersistentCpuContext());
        g_dvdInitialized = true;
        initializing = false;
        return;
    }
#endif
    // 4. Scan Files
    const fs::path& rootPath = GetDvdRoot();
    
    // Map "<dvd_root>/files" -> "/"
    ScanDirectory(rootPath / "files", "/");

    // Map "<dvd_root>/sys" -> "/sys/" (e.g. main.dol, bi2.bin)
    ScanDirectory(rootPath / "sys", "/sys/");

    const auto& overlays = RuntimeRiivolution::Overlays();
    if (overlays.empty() && RuntimeProduct::IsRetroRewind()) {
        RT_LOG(RT_TAG_DVD) << "WARNING: no Retro Rewind overlay root was found. "
                     "File replacements (menu archives, karts, drivers, courses) will not apply "
                     "and the game will look and play like the unmodded disc. Set "
                     "[paths] retro_rewind_root in Config.toml "
                     "with the RetroRewind6 folder."
                  << std::endl;
    }
    // RegisterFileEntry lets the last registration win, so apply the roots in
    // reverse discovery order: the explicitly configured root outranks the mod
    // manifest.
    const size_t vanillaEntryCount = g_fileEntries.size();
    for (auto overlay = overlays.rbegin(); overlay != overlays.rend(); ++overlay) {
        ScanOverlayRoot(*overlay);
    }
    RT_LOG(RT_TAG_DVD) << "disc index: " << vanillaEntryCount << " disc file(s), "
              << (g_fileEntries.size() - vanillaEntryCount) << " overlay registration(s) from "
              << overlays.size() << " root(s)" << std::endl;

    // Load FST mapping so real files keep their physical disc extents.
    LoadFstIndex();
    BuildAndPublishRuntimeFst();

    // Initialize the translated DVD filesystem so it can use the published FST.
    constexpr uint32_t kDvdFsInitAddress = 0x8015DF1C;
    if (TranslatedFunctionRegistry::FindByAddressPtr(kDvdFsInitAddress)) {
        InvokeIndirectCpu(kDvdFsInitAddress, &GetPersistentCpuContext());
    }

    g_dvdInitialized = true;
    initializing = false;
}
PPC_NATIVE_OVERRIDE_VOID(8015EA1C, DVDInit_8015EA1C, (), ());

// FST-only DVD functions run translated so mods can override them safely.

// 0x8015E834 -> DVDReadPrio
extern "C" int32_t DVDReadPrio_8015E834(uint32_t fileInfoPtr, uint32_t bufferPtr, int32_t length, int32_t offset, int32_t prio)
{
    (void)prio;
#if defined(__ANDROID__)
    if (g_dvdUseNod) {
        // fileInfo is in guest RAM; GetPointer gives host alias
        auto* fi = reinterpret_cast<DVDFileInfo*>(Memory::GetPointer(fileInfoPtr, sizeof(DVDFileInfo)));
        if (!fi) return -1;
        // aurora expects host pointer for dst
        void* dst = Memory::GetPointer(bufferPtr, length > 0 ? (size_t)length : 1);
        if (length > 0 && !dst) return -1;
        auto it = g_nodOffsetToEntry.find(fi->startAddr);
        if (it != g_nodOffsetToEntry.end()) {
            DVDFileInfo tmp{};
            if (DVDFastOpen(it->second, &tmp)) {
                int32_t r2 = DVDReadPrio(&tmp, dst, length, offset, prio);
                DVDClose(&tmp);
                if (r2 >= 0 && length > 0) GxNotifyGuestRamDmaWrite(bufferPtr, (uint32_t)r2);
                if (r2 >= 0) {
                    try { Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_TRANSFERRED, (uint32_t)r2); Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END); } catch(...) {}
                }
                return r2;
            } else {
            }
        } else {
        }
        uint64_t absOff = uint64_t(fi->startAddr) * 4ull + uint64_t(offset < 0 ? 0 : offset);
        const int32_t bytesRead = aurora_dvd_read_partition(
            dst, static_cast<uint32_t>(length < 0 ? 0 : length), absOff);
        const bool ok = bytesRead == length;
        if (!ok) return -1;
        if (length > 0) GxNotifyGuestRamDmaWrite(bufferPtr, (uint32_t)length);
        try { Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_TRANSFERRED, (uint32_t)length); Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END); } catch(...) {}
        return length;
    }
#endif

    uint32_t startWords = 0;
    try {
        startWords = Memory::Read32(fileInfoPtr + DVD_FILEINFO_OFFSET_ADDR);
    } catch (const Memory::AccessViolation&) {
        return DvdReadFatal(fileInfoPtr, "<invalid DVD file info>", offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "DVD file info is outside guest memory");
    }

    // Treat startAddr as an FST offset, including offsets inside a file.
    const uint64_t startBytes = static_cast<uint64_t>(startWords) * 4ull;
    const PublishedExtent* extent = FindPublishedExtentForByteOffset(startBytes);
    if (!extent) {
        return DvdReadFatal(fileInfoPtr, "<unresolved DVD file handle>", offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "DVD file info does not reference a published FST extent");
    }

    const DVDFileEntry& entry = g_fileEntries[extent->entryIndex];

    if (offset < 0 || length < 0) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "negative DVD read offset or length");
    }

    const uint64_t extentBias = startBytes - extent->startBytes;
    const uint64_t requestedOffset = extentBias + static_cast<uint32_t>(offset);
    uint32_t uLength = (uint32_t)length;

    if (requestedOffset >= entry.size) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            "read offset is outside the indexed DVD file");
    }
    const uint32_t uOffset = static_cast<uint32_t>(requestedOffset);

    if (uLength > entry.size - uOffset) {
        uLength = entry.size - uOffset;
    }

    if (uLength != 0 && !Memory::Contains(bufferPtr, uLength)) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            "DVD read destination is outside guest memory");
    }

    std::vector<uint8_t> tempBuf;
    DvdReadContract::HostReadFailure failure;
    if (!DvdReadContract::ReadExact(entry.hostPath, uOffset, uLength, tempBuf, failure)) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            DvdReadContract::Describe(failure));
    }

    CopyToGuestAsDma(bufferPtr, tempBuf.data(), uLength);

    Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_TRANSFERRED, uLength);
    Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
    CompleteDvdCancelState();

    return static_cast<int32_t>(uLength);
}
PPC_NATIVE_OVERRIDE(8015E834, DVDReadPrio_8015E834, int32_t, (uint32_t f, uint32_t b, int32_t l, int32_t o, int32_t p), (f, b, l, o, p));

// 0x8015E74C -> DVDReadAsyncPrio (internal)
// HLE: perform synchronous read and invoke callback immediately.
extern "C" int32_t DVD__ReadAsyncPrio_HLE_8015e74c(uint32_t fileInfoPtr,
                                                   uint32_t bufferPtr,
                                                   int32_t length,
                                                   int32_t offset,
                                                   uint32_t callbackPtr,
                                                   int32_t prio)
{
    const int32_t bytesRead = DVDReadPrio_8015E834(fileInfoPtr, bufferPtr, length, offset, prio);

    InvokeDvdCallback(callbackPtr, bytesRead, fileInfoPtr);
    CompleteDvdCancelState();
    return bytesRead >= 0 ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(8015E74C, DVD__ReadAsyncPrio_HLE_8015e74c, int32_t,
         (uint32_t f, uint32_t b, int32_t l, int32_t o, uint32_t cb, int32_t p),
         (f, b, l, o, cb, p));

// 0x801628CC -> DVDReadAbsAsyncPrio (internal)
// HLE: resolve the absolute disc offset to a host file and read it.
extern "C" int32_t DVD__ReadAbsAsyncPrio_HLE_801628cc(uint32_t cmdBlockPtr,
                                                      uint32_t bufferPtr,
                                                      int32_t length,
                                                      int32_t offset,
                                                      uint32_t callbackPtr,
                                                      int32_t prio)
{
    (void)prio;
#if defined(__ANDROID__)
    if (g_dvdUseNod) {
        void* dst = Memory::GetPointer(bufferPtr, length > 0 ? (size_t)length : 1);
        if (length>0 && !dst) return 0;
        const uint64_t absoluteOffset = static_cast<uint32_t>(offset < 0 ? 0 : offset);
        const int32_t bytesRead = aurora_dvd_read_partition(
            dst, static_cast<uint32_t>(length < 0 ? 0 : length), absoluteOffset);
        const bool ok = bytesRead == length;
        if (ok && dst && length>0) GxNotifyGuestRamDmaWrite(bufferPtr, (uint32_t)length);
        try { Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END); Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_TRANSFERRED, ok ? (uint32_t)length : 0); } catch(...) {}
        if (callbackPtr) InvokeDvdCallback(callbackPtr, ok?length:-1, cmdBlockPtr);
        CompleteDvdCancelState();
        return ok?1:0;
    }
#endif
    int32_t bytesRead = -1;
    {
        AbsReadResult readInfo;
        const uint32_t requestedLength = length > 0 ? static_cast<uint32_t>(length) : 0;
        const uint32_t absoluteOffset = static_cast<uint32_t>(offset);
        if (length < 0) {
            bytesRead = DvdReadFatal(cmdBlockPtr, "<unmapped DVD offset>", absoluteOffset, 0,
                                     "negative absolute DVD read length");
        } else if (!ResolveAbsRead(absoluteOffset, requestedLength, readInfo)) {
            bytesRead = DvdReadFatal(cmdBlockPtr, "<unmapped DVD offset>", absoluteOffset,
                                     requestedLength,
                                     "absolute DVD read offset is not mapped to a host file");
        } else if (requestedLength != 0 && readInfo.readLength != requestedLength) {
            bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                     readInfo.fileOffset, requestedLength,
                                     "requested range extends beyond the indexed DVD file");
        } else if (requestedLength != 0 && !Memory::Contains(bufferPtr, requestedLength)) {
            bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                     readInfo.fileOffset, requestedLength,
                                     "DVD read destination is outside guest memory");
        } else {
            std::vector<uint8_t> tempBuf;
            DvdReadContract::HostReadFailure failure;
            if (!DvdReadContract::ReadExact(readInfo.entry->hostPath,
                                            readInfo.fileOffset,
                                            readInfo.readLength,
                                            tempBuf,
                                            failure)) {
                bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                         readInfo.fileOffset, readInfo.readLength,
                                         DvdReadContract::Describe(failure));
            } else {
                CopyToGuestAsDma(bufferPtr, tempBuf.data(), tempBuf.size());
                bytesRead = static_cast<int32_t>(tempBuf.size());
            }
        }

        if (bytesRead >= 0) {
            try {
                Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
                Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_TRANSFERRED,
                                static_cast<uint32_t>(bytesRead));
            } catch (const Memory::AccessViolation&) {
            }
        }
    }

    InvokeDvdCallback(callbackPtr, bytesRead, cmdBlockPtr);
    CompleteDvdCancelState();
    return bytesRead >= 0 ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(801628CC, DVD__ReadAbsAsyncPrio_HLE_801628cc, int32_t,
         (uint32_t cb, uint32_t b, int32_t l, int32_t o, uint32_t cbfn, int32_t p),
         (cb, b, l, o, cbfn, p));


// ============================================================================
// Low-Level / Core DVD (The "Magic" Handlers)
// ============================================================================

// 0x80164848 -> DVDLowInit
extern "C" int32_t DVDLowInit_80164848() {
    // Just ensure init is done
    DVDInit_8015EA1C();
    // Translated DVDLowInit reads the /dev/di fd back from -29464
    // (0x8037F920) into its caller's locals; restore it here as well in
    // case the guest re-runs init after boot cleared the DVDs seeds.
    try {
        if (Memory::Read32(0x8034355Cu) != 0xFEEBDAEDu)
            Memory::Write32(0x8034355Cu, 0xFEEBDAEDu);
        Memory::Write32(0x8038CC00u - 29464u, 5);
    } catch (...) {}
    return 1;
}
PPC_NATIVE_OVERRIDE(80164848, DVDLowInit_80164848, int32_t, (), ());

// Throttled diagnostic: log the command blocks the translated DVD state
// machine issues (Inquiry vs ReadDiskID vs Read) so arrivals after the first
// Inquiry stay visible without spamming per-retrace.
static void TraceDvdLowCommandOnce(const char* who, uint32_t cmdBlockPtr, uint32_t callback) {
    static std::unordered_map<std::string, uint64_t> counts;
    const std::string key = who ? who : "";
    if ((++counts[key] & 511u) != 1u) return;
    uint32_t w[12] = {};
    try {
        if (cmdBlockPtr && Memory::Contains(cmdBlockPtr, 0x30u)) {
            for (int i = 0; i < 12; ++i) w[i] = Memory::Read32(cmdBlockPtr + i * 4u);
        }
    } catch (...) {}
    RT_LOGF(RT_TAG_DVD, "%s: block=0x%08x cb=0x%08x w0=%08x w1=%08x w2=%08x w3=%08x w4=%08x w5=%08x w6=%08x w7=%08x w8=%08x w9=%08x wa=%08x wb=%08x\n",
            who, cmdBlockPtr, callback,
            w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
}

extern "C" int32_t DVDLowInquiry_80165A30(uint32_t cmdBlockPtr, uint32_t callback)
{
    TraceDvdLowCommandOnce("DVDLowInquiry", cmdBlockPtr, callback);
#if defined(__ANDROID__)
    if (g_dvdUseNod) {
        // The translated DVD::InquiryAsync state machine copies the drive info
        // struct (8B) from its command block, so the info pointer must be
        // readable guest memory. Route the inquiry through the nod shim with
        // a scratch guest buffer instead of a null info pointer.
        auto* block = cmdBlockPtr ? reinterpret_cast<DVDCommandBlock*>(Memory::GetPointer(cmdBlockPtr, sizeof(DVDCommandBlock))) : nullptr;
        // Scratch DVDDriveInfo inside MEM1 arena (32B aligned guest buffer).
        constexpr uint32_t kInquiryScratch = 0x80390000u;
        int ok = 0;
        if (Memory::Contains(kInquiryScratch, 32u)) {
            for (uint32_t off = 0; off < 32u; off += 4u) {
                Memory::Write32(kInquiryScratch + off, 0);
            }
            auto* info = reinterpret_cast<DVDDriveInfo*>(Memory::GetPointer(kInquiryScratch, sizeof(DVDDriveInfo)));
            ok = DVDInquiryAsync(block, info, nullptr);
        } else {
            ok = DVDInquiryAsync(block, nullptr, nullptr);
        }
        if (block) { block->state = 0; }
        if (callback) {
            // The callback's branch decision reads the seeded drive state
            // directly ((r31 & 2) re-issue test uses the status we pass, but
            // the (r31 & 1) advance test and the -25884 gates read memory).
            // Pass the status matching seeded state 14: on hardware this is
            // "drive ready, no cover event" whose DI status is 0x0 with the
            // transfer-complete bit set on completion — but the callback's
            // own (r31 & 1) test needs bit 0. Pass 0x1 (advance, no re-issue).
            uint32_t seeded = 0;
            try { seeded = Memory::Read32(0x803866E4u); } catch (...) {}
            RT_LOGF(RT_TAG_DVD, "DVDLowInquiry: cb=0x%08x ok=%d seededState=%u\n",
                    callback, ok, seeded);
            // The func_80161EEC entry gate (loc_80161F90/80161F9C) only takes
            // the completion path when the drive state at -25884 is 3 or 15;
            // anything else (e.g. the 14 left by the pump) diverts to the
            // loc_801620EC error path which exits without re-issuing. On real
            // hardware the DI interrupt arrives with the state the issuer left
            // (cover-closed idle = 3), so restore that here. The ReadDiskID
            // re-issue at loc_80162070 compares -25976 (0x80386688) and
            // -25972 (0x8038668C) against 0 and EXITS on nonzero
            // (loc_8016208C/80162098): both must stay 0 (DVDInit default).
            try {
                Memory::Write32(0x803866E4u, 3);
                Memory::Write32(0x80386688u, 0);
                Memory::Write32(0x8038668Cu, 0);
            } catch (...) {}
            constexpr uint32_t kDiAdvance = 0x1u;
            InvokeDvdLowCallback(callback, ok ? kDiAdvance : 0u);
        }
        CompleteDvdCancelState();
        return ok;
    }
#endif
    // HLE: acknowledge the drive is present. Must mark the command block complete
    // (State = 0) or callers polling this address hang waiting for "Busy" to clear.
    if (cmdBlockPtr) {
        Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
    }
    CompleteDvdCancelState();

    return 1; // Return 1 to indicate the command was successfully issued.
}
PPC_NATIVE_OVERRIDE(80165A30, DVDLowInquiry_80165A30, int32_t, (uint32_t b, uint32_t c), (b, c));

// 0x80164AAC -> DVDLowReadDiskID
extern "C" int32_t DVDLowReadDiskID_80164AAC(uint32_t diskIdPtr, uint32_t callback) {
    // Unthrottled: this is the signal that cover-wait exited. Log every call.
    RT_LOGF(RT_TAG_DVD, "DVDLowReadDiskID: id=0x%08x cb=0x%08x\n", diskIdPtr, callback);
    TraceDvdLowCommandOnce("DVDLowReadDiskID", diskIdPtr, callback);
#if defined(__ANDROID__)
    if (g_dvdUseNod) {
        DVDDiskID* id = diskIdPtr ? reinterpret_cast<DVDDiskID*>(Memory::GetPointer(diskIdPtr, sizeof(DVDDiskID))) : nullptr;
        int ok = DVDReadDiskID(nullptr, id, nullptr);
        if (callback) {
            // Same DI status 0x3 as Inquiry: the translated ReadDiskID
            // callback shares the func_80161EEC branch structure ((r31 & 2)
            // re-issue, (r31 & 1) advance).
            constexpr uint32_t kDiReady = 0x3u;
            InvokeDvdLowCallback(callback, ok ? kDiReady : 0u);
        }
        CompleteDvdCancelState();
        return ok;
    }
#endif
    if (diskIdPtr) {
        Memory::Write32(diskIdPtr + 0x00, CurrentDiscGameCode());
        Memory::Write16(diskIdPtr + 0x04, 0x3031);
        Memory::Write8 (diskIdPtr + 0x06, 0x01);
    }
    if (callback) {
        constexpr uint32_t kDiReady = 0x3u;
        InvokeDvdLowCallback(callback, kDiReady);
    }
    CompleteDvdCancelState();
    return 1; // Success
}
PPC_NATIVE_OVERRIDE(80164AAC, DVDLowReadDiskID_80164AAC, int32_t, (uint32_t p, uint32_t c), (p, c));

// 0x80166330 -> DVDLowRead (And 0x80165708 UnencryptedRead)
// The game calls this to read the Disk Header (offset 0) or raw data.
// Same DI status 0x3 as Inquiry/ReadDiskID: the translated read callback
// shares the func_80161EEC branch structure ((r31 & 2) re-issue,
// (r31 & 1) advance).
static constexpr uint32_t kDiTransferComplete = 0x3u;

extern "C" int32_t DVDLowRead_80166330(uint32_t buffer, uint32_t length, uint32_t offset, uint32_t callback)
{
    // Throttled like the command trace: visible if reads ever start.
    {
        static uint64_t n = 0;
        if ((++n & 511u) == 1u) {
            RT_LOGF(RT_TAG_DVD, "DVDLowRead: buf=0x%08x len=0x%x off=0x%x cb=0x%08x\n",
                    buffer, length, offset, callback);
        }
    }
#if defined(__ANDROID__)
    if (g_dvdUseNod) {
        if (offset == 0 && length >= 0x20) {
            void* dst = length ? Memory::GetPointer(buffer, length) : nullptr;
            if (length && !dst) return 0;
            if (!Memory::Contains(buffer, length)) {
                if (callback) InvokeDvdLowCallback(callback, DvdReadContract::kInterruptDriveError);
                CompleteDvdCancelState();
                return 0;
            }
            Memory::Write32(buffer + 0x00, CurrentDiscGameCode());
            Memory::Write16(buffer + 0x04, 0x3031);
            if (dst) GxNotifyGuestRamDmaWrite(buffer, length);
            if (callback) InvokeDvdLowCallback(callback, kDiTransferComplete);
            CompleteDvdCancelState();
            return 1;
        }
        void* dst = length ? Memory::GetPointer(buffer, length) : nullptr;
        if (length && !dst) return 0;
        const int32_t bytesRead = aurora_dvd_read_partition(dst, length, offset);
        const bool ok = bytesRead == static_cast<int32_t>(length);
        if (ok && dst) GxNotifyGuestRamDmaWrite(buffer, length);
        if (callback) InvokeDvdLowCallback(callback, ok ? kDiTransferComplete : 0u);
        CompleteDvdCancelState();
        return ok;
    }
#endif
    const auto finish = [callback](bool succeeded) {
        const auto completion = DvdReadContract::CompletionFor(succeeded);
        InvokeDvdLowCallback(callback, completion.callbackResult);
        CompleteDvdCancelState();
        return completion.returnValue;
    };

    if (length == 0) {
        return finish(true);
    }

    if (offset == 0 && length >= 0x20) {
        if (!Memory::Contains(buffer, length)) {
            ReportDvdReadError("<synthetic disc header>", offset, length,
                               "DVD read destination is outside guest memory");
            return finish(false);
        }
        Memory::Write32(buffer + 0x00, CurrentDiscGameCode());
        Memory::Write16(buffer + 0x04, 0x3031);     // 01
        GxNotifyGuestRamDmaWrite(buffer, length);
        return finish(true);
    }

    AbsReadResult readInfo;
    if (!ResolveAbsRead(offset, length, readInfo)) {
        ReportDvdReadError("<unmapped DVD offset>", offset, length,
                           "absolute DVD read offset is not mapped to a host file");
        return finish(false);
    }
    if (readInfo.readLength != length) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset, length,
                           "requested range extends beyond the indexed DVD file");
        return finish(false);
    }
    if (!Memory::Contains(buffer, length)) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset, length,
                           "DVD read destination is outside guest memory");
        return finish(false);
    }

    std::vector<uint8_t> tempBuf;
    DvdReadContract::HostReadFailure failure;
    if (!DvdReadContract::ReadExact(readInfo.entry->hostPath,
                                    readInfo.fileOffset,
                                    readInfo.readLength,
                                    tempBuf,
                                    failure)) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset,
                           readInfo.readLength, DvdReadContract::Describe(failure));
        return finish(false);
    }

    CopyToGuestAsDma(buffer, tempBuf.data(), tempBuf.size());
    return finish(true);
}
PPC_NATIVE_OVERRIDE(80166330, DVDLowRead_80166330, int32_t, (uint32_t b, uint32_t l, uint32_t o, uint32_t c), (b, l, o, c));

// UnencryptedRead has the same completion and failure contract as DVDLowRead.
extern "C" int32_t DVDLowUnencryptedRead_80165708(uint32_t b, uint32_t l, uint32_t o, uint32_t c) {
    return DVDLowRead_80166330(b, l, o, c);
}
PPC_NATIVE_OVERRIDE(80165708, DVDLowUnencryptedRead_80165708, int32_t, (uint32_t b, uint32_t l, uint32_t o, uint32_t c), (b, l, o, c));

// ============================================================================
// Other Necessary Stubs
// ============================================================================

extern "C" int32_t DVDCheckDevice_801643FC() { return 1; } // Ready
PPC_NATIVE_OVERRIDE(801643FC, DVDCheckDevice_801643FC, int32_t, (), ());

extern "C" int32_t DVDLowClearCoverInterrupt_80166964(uint32_t cb) { return 1; }
PPC_NATIVE_OVERRIDE(80166964, DVDLowClearCoverInterrupt_80166964, int32_t, (uint32_t cb), (cb));

