#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace kharvoxnative::scan {

// PE timestamp + SizeOfImage for a loaded module. Used to guard
// hardcoded/signature-resolved offsets against running on an unexpected
// game build — see docs/FORBIDDEN-BOUNDARIES.md item 6.
struct ModuleBuildInfo {
    std::uint32_t timestamp{0};
    std::uint32_t size_of_image{0};

    friend bool operator==(const ModuleBuildInfo&, const ModuleBuildInfo&) = default;
};

// Reads the PE timestamp and SizeOfImage from a loaded module's headers.
std::optional<ModuleBuildInfo> module_build_info(const wchar_t* module_name);

// Scans a module for a byte pattern (e.g. "48 8B ?? ?? 89", '?'/'??' =
// wildcard byte). Returns nullopt if the module isn't loaded, the pattern
// is empty, or there is not EXACTLY one match anywhere in the module image.
// An ambiguous pattern is treated as a failure, never "first match wins" —
// a hook built on a non-unique signature is not trustworthy.
std::optional<std::uintptr_t> module_pattern(const wchar_t* module_name, std::string_view pattern);

// High-level, fail-closed offset resolver for hooks/reads that must never
// run against a mismatched build. Resolves `pattern` inside `module_name`,
// optionally checks the module's build info against `expected_build` (pass
// nullopt while a signature is still being proven out and no reference
// build has been recorded yet), and logs the outcome under `name` via
// kharvoxnative::log. Returns nullopt on ANY failure — module not found, build
// mismatch, pattern not found, or pattern ambiguous — never a best-effort
// address.
std::optional<std::uintptr_t> resolve_signature(
    const wchar_t* module_name,
    std::string_view name,
    std::string_view pattern,
    std::optional<ModuleBuildInfo> expected_build = std::nullopt);

}  // namespace kharvoxnative::scan
