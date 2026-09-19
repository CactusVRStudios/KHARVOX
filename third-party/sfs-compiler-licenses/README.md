## SFS shader compiler

The optional `KHARVOX_BUILD_SFS_COMPILER` build statically links unmodified
SPIRV-Cross core/GLSL libraries (KhronosGroup/SPIRV-Cross,
`vulkan-sdk-1.4.304.0`, commit `ebe2aa0cd80f5eb5cd8a605da604cacf72205f3b`),
glslang and SPIRV-Tools. KHARVOX's typed transformation is in
`src/sfs/ShaderCompiler.cpp`; it is not a modification of those libraries.
Full license collections are retained in `third-party/sfs-compiler-licenses`
and must accompany binary builds with this option enabled.

The local experimental build uses glslang headers reporting 16.5.0 from the
user's installed `D:/DoomVR/build/shader-tools/glslang-main` SDK. Its exact
upstream build revision and the bundled SPIRV-Tools revision are not recorded
in that installation; do not represent this as a reproducibly pinned toolchain.
License texts for those libraries were obtained from their official upstream
repositories on 2026-09-17. A distributable release should rebuild them from
pinned sources and retain the matching notices.

The local DOOM shader capture and the user-provided shader replacement profile
are test inputs, not part of this source distribution. The native SFS layer
does not load an external stereo provider DLL. Profile provenance and its
retained notice are documented in `Docs/THIRD_PARTY_NOTICES.txt`.
