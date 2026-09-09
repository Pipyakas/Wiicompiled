#pragma once
#include <filesystem>
#include <optional>
namespace AndroidFilesDir {
std::filesystem::path Get() noexcept;
void Set(std::filesystem::path p) noexcept;
inline std::optional<std::filesystem::path> GetOpt() noexcept {
    auto p = Get();
    if (p.empty()) return std::nullopt;
    return p;
}
}
