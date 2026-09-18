#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kharvox::sfs {
struct ShaderCompileOptions {
    bool computeStereo{};
    bool vertexProjection{};
    bool profileReplacement{};
    bool screenSpaceUi{};
    // Indirect group counts stay GPU-owned. A fixed-eye variant executes the
    // original dispatch grid once for its own layer, without rewriting counts.
    int indirectEye{-1};
    // One-layer auxiliary views keep their own projection/reconstruction.
    // Descriptor-array promotion is still required by the shared image layout.
    bool monoscopicView{};
};
struct CompiledShader {
    std::vector<uint32_t> words;
    std::string glsl;
    unsigned clusterCorrections{}, worldCorrections{}, refractionCorrections{}, temporalCorrections{}, ssdoCorrections{};
    bool vertexProjectionApplied{};
    bool screenSpaceUiApplied{};
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
