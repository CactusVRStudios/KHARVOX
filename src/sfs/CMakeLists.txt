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
endif()
