#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kharvox::sfs {
struct ShaderCompileOptions {
    bool computeStereo{};
    bool vertexProjection{};
    bool profileReplacement{};
};
struct CompiledShader {
    std::vector<uint32_t> words;
    std::string glsl;
    unsigned clusterCorrections{}, worldCorrections{};
    bool vertexProjectionApplied{};
    // Descriptor bindings whose original 2D view must become an array view.
    struct Binding {uint32_t set{},binding{};bool storage{},depth{};};
    std::vector<Binding> arrayBindings;
};
// Throws on unsupported transformations or compilation failure; no mono fallback.
CompiledShader compileStereoShader(const std::vector<uint32_t>& original,
                                  const ShaderCompileOptions& options={});
bool hasStereoStorageOutput(const std::vector<uint32_t>& original);
bool needsStereoProjection(const std::vector<uint32_t>& original,bool profileReplacement);
}
