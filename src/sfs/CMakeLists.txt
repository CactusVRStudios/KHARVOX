set(KHARVOX_SPIRV_CROSS_SOURCE "" CACHE PATH "SPIRV-Cross vulkan-sdk-1.4.304.0 source directory")
set(KHARVOX_SPIRV_CROSS_LIB "" CACHE PATH "SPIRV-Cross Release static library directory")
set(KHARVOX_GLSLANG_ROOT "" CACHE PATH "Installed glslang headers and static libraries")
if(NOT EXISTS "${KHARVOX_SPIRV_CROSS_SOURCE}/spirv_glsl.hpp" OR NOT EXISTS "${KHARVOX_GLSLANG_ROOT}/include/glslang/Include/glslang_c_interface.h")
  message(FATAL_ERROR "SFS compiler requires explicit SPIRV-Cross source/library and glslang install paths")
endif()
add_library(KharvoxSfsCompiler STATIC ShaderCompiler.cpp)
target_compile_features(KharvoxSfsCompiler PUBLIC cxx_std_17)
target_include_directories(KharvoxSfsCompiler PRIVATE "${KHARVOX_SPIRV_CROSS_SOURCE}" "${KHARVOX_GLSLANG_ROOT}/include")
foreach(lib spirv-cross-glsl spirv-cross-core)
  target_link_libraries(KharvoxSfsCompiler PUBLIC "${KHARVOX_SPIRV_CROSS_LIB}/${lib}.lib")
endforeach()
foreach(lib glslang glslang-default-resource-limits MachineIndependent GenericCodeGen OSDependent SPIRV SPIRV-Tools-opt SPIRV-Tools)
  target_link_libraries(KharvoxSfsCompiler PUBLIC "${KHARVOX_GLSLANG_ROOT}/lib/${lib}.lib")
endforeach()
add_executable(KharvoxSfsCompile ../../tools/sfs_compile.cpp)
target_link_libraries(KharvoxSfsCompile PRIVATE KharvoxSfsCompiler)
if(BUILD_TESTING AND WIN32)
  add_executable(KharvoxSfsGroupVoteTests ../../tests/sfs_group_vote_tests.cpp)
  target_link_libraries(KharvoxSfsGroupVoteTests PRIVATE KharvoxSfsCompiler)
  target_include_directories(KharvoxSfsGroupVoteTests PRIVATE "${KHARVOX_SPIRV_CROSS_SOURCE}")
  add_test(NAME sfs-legacy-group-vote COMMAND KharvoxSfsGroupVoteTests)
  add_executable(KharvoxSfsGameplayContractTests ../../tests/sfs_gameplay_contract_tests.cpp)
  target_compile_features(KharvoxSfsGameplayContractTests PRIVATE cxx_std_17)
  add_test(NAME sfs-gameplay-contract COMMAND KharvoxSfsGameplayContractTests)
  add_executable(KharvoxSfsHandDepthGpuTests ../../tests/hand_scene_depth_gpu_tests.cpp)
  target_compile_features(KharvoxSfsHandDepthGpuTests PRIVATE cxx_std_17)
  target_compile_definitions(KharvoxSfsHandDepthGpuTests PRIVATE KHARVOX_TEST_DEPTH_LAYER=1)
  target_include_directories(KharvoxSfsHandDepthGpuTests PRIVATE "${PROJECT_SOURCE_DIR}/third-party/vulkan-headers/include")
  add_test(NAME sfs-hand-depth-right-layer-gpu COMMAND KharvoxSfsHandDepthGpuTests)
  set_tests_properties(sfs-hand-depth-right-layer-gpu PROPERTIES TIMEOUT 30)
  add_executable(KharvoxSfsProjectionTests ../../tests/sfs_projection_tests.cpp)
  target_compile_features(KharvoxSfsProjectionTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxSfsProjectionTests PRIVATE "${PROJECT_SOURCE_DIR}/third-party/openxr-sdk/include")
  add_test(NAME sfs-frame-projection COMMAND KharvoxSfsProjectionTests)
  set(sfs_fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  foreach(stage vert frag)
    add_custom_command(OUTPUT "${sfs_fixtures}/sampling.${stage}.spv"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${sfs_fixtures}"
      COMMAND "${KHARVOX_GLSLANG_ROOT}/bin/glslang.exe" -V --target-env vulkan1.1
        "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_sampling.${stage}" -o "${sfs_fixtures}/sampling.${stage}.spv"
      DEPENDS "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_sampling.${stage}" VERBATIM)
  endforeach()
  add_executable(KharvoxSfsSamplingGpuTests ../../tests/sfs_sampling_gpu_tests.cpp
    "${sfs_fixtures}/sampling.vert.spv" "${sfs_fixtures}/sampling.frag.spv")
  target_link_libraries(KharvoxSfsSamplingGpuTests PRIVATE KharvoxSfsCompiler)
  target_include_directories(KharvoxSfsSamplingGpuTests PRIVATE "${PROJECT_SOURCE_DIR}/third-party/vulkan-headers/include")
  add_test(NAME sfs-typed-sampling-gpu COMMAND KharvoxSfsSamplingGpuTests
    "${sfs_fixtures}/sampling.vert.spv" "${sfs_fixtures}/sampling.frag.spv")
  set_tests_properties(sfs-typed-sampling-gpu PROPERTIES TIMEOUT 30)
  add_executable(KharvoxSfsRuntimeGpuTests ../../tests/sfs_sampling_gpu_tests.cpp NativeSfs.cpp
    "${sfs_fixtures}/sampling.vert.spv" "${sfs_fixtures}/sampling.frag.spv")
  target_link_libraries(KharvoxSfsRuntimeGpuTests PRIVATE KharvoxSfsCompiler)
  target_compile_definitions(KharvoxSfsRuntimeGpuTests PRIVATE KHARVOX_SFS_TEST_RUNTIME KHARVOX_HAVE_SFS_COMPILER WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(KharvoxSfsRuntimeGpuTests PRIVATE "${PROJECT_SOURCE_DIR}/third-party/openxr-sdk/include" "${PROJECT_SOURCE_DIR}/third-party/vulkan-headers/include")
  add_test(NAME sfs-runtime-gpu COMMAND KharvoxSfsRuntimeGpuTests
    "${sfs_fixtures}/sampling.vert.spv" "${sfs_fixtures}/sampling.frag.spv")
  set_tests_properties(sfs-runtime-gpu PROPERTIES TIMEOUT 30)
  add_custom_command(OUTPUT "${sfs_fixtures}/affine.frag.spv"
    COMMAND "${KHARVOX_GLSLANG_ROOT}/bin/glslang.exe" -V --target-env vulkan1.1
      "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_affine.frag" -o "${sfs_fixtures}/affine.frag.spv"
    DEPENDS "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_affine.frag" VERBATIM)
  add_executable(KharvoxSfsAffineGpuTests ../../tests/sfs_sampling_gpu_tests.cpp NativeSfs.cpp
    "${sfs_fixtures}/sampling.vert.spv" "${sfs_fixtures}/affine.frag.spv")
  target_link_libraries(KharvoxSfsAffineGpuTests PRIVATE KharvoxSfsCompiler)
  target_compile_definitions(KharvoxSfsAffineGpuTests PRIVATE KHARVOX_SFS_TEST_RUNTIME KHARVOX_SFS_TEST_AFFINE KHARVOX_HAVE_SFS_COMPILER WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(KharvoxSfsAffineGpuTests PRIVATE "${PROJECT_SOURCE_DIR}/third-party/openxr-sdk/include" "${PROJECT_SOURCE_DIR}/third-party/vulkan-headers/include")
  add_test(NAME sfs-affine-ipd-gpu COMMAND KharvoxSfsAffineGpuTests
    "${sfs_fixtures}/sampling.vert.spv" "${sfs_fixtures}/affine.frag.spv")
  set_tests_properties(sfs-affine-ipd-gpu PROPERTIES TIMEOUT 30)
  foreach(kind lighting shadow)
    add_custom_command(OUTPUT "${sfs_fixtures}/${kind}.frag.spv"
      COMMAND "${KHARVOX_GLSLANG_ROOT}/bin/glslang.exe" -V --target-env vulkan1.1
        "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_${kind}.frag" -o "${sfs_fixtures}/${kind}.frag.spv"
      DEPENDS "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_${kind}.frag" VERBATIM)
    add_executable(KharvoxSfs${kind}GpuTests ../../tests/sfs_sampling_gpu_tests.cpp NativeSfs.cpp
      "${sfs_fixtures}/sampling.vert.spv" "${sfs_fixtures}/${kind}.frag.spv")
    target_link_libraries(KharvoxSfs${kind}GpuTests PRIVATE KharvoxSfsCompiler)
    string(TOUPPER "${kind}" kind_upper)
    target_compile_definitions(KharvoxSfs${kind}GpuTests PRIVATE KHARVOX_SFS_TEST_RUNTIME KHARVOX_SFS_TEST_AFFINE KHARVOX_SFS_TEST_${kind_upper} KHARVOX_HAVE_SFS_COMPILER WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(KharvoxSfs${kind}GpuTests PRIVATE "${PROJECT_SOURCE_DIR}/third-party/openxr-sdk/include" "${PROJECT_SOURCE_DIR}/third-party/vulkan-headers/include")
    add_test(NAME sfs-${kind}-gpu COMMAND KharvoxSfs${kind}GpuTests "${sfs_fixtures}/sampling.vert.spv" "${sfs_fixtures}/${kind}.frag.spv")
    set_tests_properties(sfs-${kind}-gpu PROPERTIES TIMEOUT 30)
  endforeach()
  add_test(NAME sfs-lighting-asymmetric-gpu COMMAND KharvoxSfslightingGpuTests "${sfs_fixtures}/sampling.vert.spv" "${sfs_fixtures}/lighting.frag.spv" asymmetric)
  set_tests_properties(sfs-lighting-asymmetric-gpu PROPERTIES TIMEOUT 30)
  add_custom_command(OUTPUT "${sfs_fixtures}/shadow-projection.vert.spv"
    COMMAND "${KHARVOX_GLSLANG_ROOT}/bin/glslang.exe" -V --target-env vulkan1.1
      "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_shadow_projection.vert" -o "${sfs_fixtures}/shadow-projection.vert.spv"
    DEPENDS "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_shadow_projection.vert" VERBATIM)
  add_executable(KharvoxSfsShadowProjectionGpuTests ../../tests/sfs_sampling_gpu_tests.cpp NativeSfs.cpp
    "${sfs_fixtures}/shadow-projection.vert.spv" "${sfs_fixtures}/sampling.frag.spv")
  target_link_libraries(KharvoxSfsShadowProjectionGpuTests PRIVATE KharvoxSfsCompiler)
  target_compile_definitions(KharvoxSfsShadowProjectionGpuTests PRIVATE KHARVOX_SFS_TEST_RUNTIME KHARVOX_SFS_TEST_AFFINE KHARVOX_SFS_TEST_SHADOW_PROJECTION KHARVOX_HAVE_SFS_COMPILER WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(KharvoxSfsShadowProjectionGpuTests PRIVATE "${PROJECT_SOURCE_DIR}/third-party/openxr-sdk/include" "${PROJECT_SOURCE_DIR}/third-party/vulkan-headers/include")
  add_test(NAME sfs-shadow-projection-gpu COMMAND KharvoxSfsShadowProjectionGpuTests "${sfs_fixtures}/shadow-projection.vert.spv" "${sfs_fixtures}/sampling.frag.spv")
  add_test(NAME sfs-camera-depth-projection-gpu COMMAND KharvoxSfsShadowProjectionGpuTests "${sfs_fixtures}/shadow-projection.vert.spv" "${sfs_fixtures}/sampling.frag.spv" camera)
  set_tests_properties(sfs-shadow-projection-gpu sfs-camera-depth-projection-gpu PROPERTIES TIMEOUT 30)
  add_custom_command(OUTPUT "${sfs_fixtures}/material-atlas.vert.spv"
    COMMAND "${KHARVOX_GLSLANG_ROOT}/bin/glslang.exe" -V --target-env vulkan1.1
      "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_material_atlas.vert" -o "${sfs_fixtures}/material-atlas.vert.spv"
    DEPENDS "${PROJECT_SOURCE_DIR}/tests/shaders/sfs_material_atlas.vert" VERBATIM)
  add_executable(KharvoxSfsLightingRewriteTests ../../tests/sfs_lighting_rewrite_tests.cpp "${sfs_fixtures}/material-atlas.vert.spv" "${sfs_fixtures}/shadow-projection.vert.spv")
  target_link_libraries(KharvoxSfsLightingRewriteTests PRIVATE KharvoxSfsCompiler)
  add_test(NAME sfs-lighting-rewrite COMMAND KharvoxSfsLightingRewriteTests "${sfs_fixtures}/material-atlas.vert.spv" "${sfs_fixtures}/shadow-projection.vert.spv")
endif()
