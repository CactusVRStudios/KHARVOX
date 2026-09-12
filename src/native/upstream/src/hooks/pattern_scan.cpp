#include "kharvoxnative/pattern_scan.h"
#include "kharvoxnative/log.h"
#include <Windows.h>
#include <Psapi.h>
#include <charconv>
#include <format>
#include <vector>

namespace kharvoxnative::scan {
namespace {

std::vector<int> parse(std::string_view pattern) {
    std::vector<int> bytes;
    for (size_t i = 0; i < pattern.size();) {
        while (i < pattern.size() && pattern[i] == ' ') ++i;
        if (i >= pattern.size()) break;
        if (pattern[i] == '?') {
            bytes.push_back(-1);
            ++i;
            if (i < pattern.size() && pattern[i] == '?') ++i;
            continue;
        }
        if (i + 1 >= pattern.size()) break;
        unsigned value{};
        auto first = pattern.data() + i;
        auto last = first + 2;
        if (std::from_chars(first, last, value, 16).ec == std::errc()) bytes.push_back(static_cast<int>(value));
        i += 2;
    }
    return bytes;
}

// Collects every match instead of stopping at the first one so callers can
// detect and reject an ambiguous signature rather than silently trusting
// whichever match happens to come first in the image.
std::vector<std::uintptr_t> find_all(std::uint8_t* base, size_t size, const std::vector<int>& sig) {
    std::vector<std::uintptr_t> hits;
    if (sig.empty() || sig.size() > size) return hits;
    for (size_t i = 0; i <= size - sig.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < sig.size(); ++j) {
            if (sig[j] >= 0 && base[i + j] != static_cast<std::uint8_t>(sig[j])) { ok = false; break; }
        }
        if (ok) hits.push_back(reinterpret_cast<std::uintptr_t>(base + i));
    }
    return hits;
}

}  // namespace

std::optional<ModuleBuildInfo> module_build_info(const wchar_t* module_name) {
    HMODULE mod = GetModuleHandleW(module_name);
    if (!mod) return std::nullopt;

    auto* base = reinterpret_cast<std::uint8_t*>(mod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return std::nullopt;

    ModuleBuildInfo info;
    info.timestamp = nt->FileHeader.TimeDateStamp;
    info.size_of_image = nt->OptionalHeader.SizeOfImage;
    return info;
}

std::optional<std::uintptr_t> module_pattern(const wchar_t* module_name, std::string_view pattern) {
    HMODULE mod = GetModuleHandleW(module_name);
    if (!mod) return std::nullopt;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) return std::nullopt;

    const auto sig = parse(pattern);
    auto* base = static_cast<std::uint8_t*>(mi.lpBaseOfDll);
    const auto hits = find_all(base, mi.SizeOfImage, sig);
    if (hits.size() != 1) return std::nullopt;
    return hits.front();
}

std::optional<std::uintptr_t> resolve_signature(const wchar_t* module_name,
                                                  std::string_view name,
                                                  std::string_view pattern,
                                                  std::optional<ModuleBuildInfo> expected_build) {
    HMODULE mod = GetModuleHandleW(module_name);
    if (!mod) {
        log::warn(std::format("resolve_signature[{}]: module not loaded", name));
        return std::nullopt;
    }

    if (expected_build) {
        const auto actual = module_build_info(module_name);
        if (!actual || !(*actual == *expected_build)) {
            log::error(std::format(
                "resolve_signature[{}]: build mismatch, refusing to resolve offset "
                "(expected timestamp={:#x} size={:#x}, got timestamp={:#x} size={:#x})",
                name, expected_build->timestamp, expected_build->size_of_image,
                actual ? actual->timestamp : 0u, actual ? actual->size_of_image : 0u));
            return std::nullopt;
        }
    }

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) {
        log::warn(std::format("resolve_signature[{}]: GetModuleInformation failed", name));
        return std::nullopt;
    }

    const auto sig = parse(pattern);
    auto* base = static_cast<std::uint8_t*>(mi.lpBaseOfDll);
    const auto hits = find_all(base, mi.SizeOfImage, sig);

    if (hits.empty()) {
        log::error(std::format("resolve_signature[{}]: signature not found", name));
        return std::nullopt;
    }
    if (hits.size() > 1) {
        log::error(std::format("resolve_signature[{}]: signature ambiguous ({} matches), refusing to guess",
                                name, hits.size()));
        return std::nullopt;
    }

    const auto rva = hits.front() - reinterpret_cast<std::uintptr_t>(base);
    log::info(std::format("resolve_signature[{}]: resolved to module+{:#x}", name, rva));
    return hits.front();
}

}  // namespace kharvoxnative::scan
