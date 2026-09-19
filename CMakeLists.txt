cmake_minimum_required(VERSION 3.20)
project(KHARVOX VERSION 1.0.0 LANGUAGES C CXX)

include(CTest)
option(KHARVOX_BUILD_SFS_COMPILER "Build the native SFS shader transformer" OFF)
if(KHARVOX_BUILD_SFS_COMPILER)
  add_subdirectory(src/sfs)
endif()
if(BUILD_TESTING)
  add_executable(KharvoxSfsResourceTests tests/sfs_resource_tests.cpp)
  target_compile_features(KharvoxSfsResourceTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxSfsResourceTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME sfs-resources COMMAND KharvoxSfsResourceTests)
  add_executable(KharvoxSfsShaderIdentityTests tests/sfs_shader_identity_tests.cpp)
  target_compile_features(KharvoxSfsShaderIdentityTests PRIVATE cxx_std_17)
  add_test(NAME sfs-shader-identity COMMAND KharvoxSfsShaderIdentityTests)
  add_executable(KharvoxSfsPipelineIdentityTests tests/sfs_pipeline_identity_tests.cpp)
  target_compile_features(KharvoxSfsPipelineIdentityTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxSfsPipelineIdentityTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME sfs-pipeline-identity COMMAND KharvoxSfsPipelineIdentityTests)
  add_executable(KharvoxSfsProfileAudit tools/sfs_profile_audit.cpp)
  target_compile_features(KharvoxSfsProfileAudit PRIVATE cxx_std_17)
  target_include_directories(KharvoxSfsProfileAudit PRIVATE third-party/vulkan-headers/include)
  if(WIN32)
    add_executable(KharvoxSfsMultiviewGpuTests tests/sfs_multiview_gpu_tests.cpp)
    target_compile_features(KharvoxSfsMultiviewGpuTests PRIVATE cxx_std_17)
    target_include_directories(KharvoxSfsMultiviewGpuTests PRIVATE third-party/vulkan-headers/include)
    add_test(NAME sfs-multiview-gpu COMMAND KharvoxSfsMultiviewGpuTests)
    set_tests_properties(sfs-multiview-gpu PROPERTIES TIMEOUT 30)
  endif()
endif()
if(BUILD_TESTING)
  add_executable(KharvoxLaserSourceTests tests/laser_source_tests.cpp)
  target_compile_features(KharvoxLaserSourceTests PRIVATE cxx_std_17)
  add_test(NAME laser-source COMMAND KharvoxLaserSourceTests)
  if(WIN32)
    add_executable(KharvoxVirtualTextureGuardTests tests/virtual_texture_guard_tests.cpp)
    target_compile_features(KharvoxVirtualTextureGuardTests PRIVATE cxx_std_17)
    add_test(NAME virtual-texture-guard COMMAND KharvoxVirtualTextureGuardTests)
  endif()
  add_executable(KharvoxCyberdemonQuadPolicyTests tests/cyberdemon_quad_policy_tests.cpp)
  target_compile_features(KharvoxCyberdemonQuadPolicyTests PRIVATE cxx_std_17)
  add_test(NAME cyberdemon-quad-policy COMMAND KharvoxCyberdemonQuadPolicyTests)
endif()

if(BUILD_TESTING)
  add_executable(KharvoxAerWeaponAttachmentTests tests/aer_weapon_attachment_tests.cpp)
  target_compile_features(KharvoxAerWeaponAttachmentTests PRIVATE cxx_std_17)
  add_test(NAME aer-weapon-attachments COMMAND KharvoxAerWeaponAttachmentTests)
  add_executable(KharvoxAerDrawModelTests tests/aer_draw_model_tests.cpp)
  target_compile_features(KharvoxAerDrawModelTests PRIVATE cxx_std_17)
  add_test(NAME aer-draw-model COMMAND KharvoxAerDrawModelTests)
  add_executable(KharvoxAerEyeBasisTurnTests tests/aer_eye_basis_turn_tests.cpp)
  target_compile_features(KharvoxAerEyeBasisTurnTests PRIVATE cxx_std_17)
  add_test(NAME aer-eye-basis-turn COMMAND KharvoxAerEyeBasisTurnTests)
  add_executable(KharvoxAerWeaponSourceTests tests/aer_weapon_source_tests.cpp)
  target_compile_features(KharvoxAerWeaponSourceTests PRIVATE cxx_std_17)
  add_test(NAME aer-weapon-source COMMAND KharvoxAerWeaponSourceTests)
  add_executable(KharvoxAerSourceTrackingTests tests/aer_source_tracking_tests.cpp)
  target_compile_features(KharvoxAerSourceTrackingTests PRIVATE cxx_std_17)
  add_test(NAME aer-source-tracking COMMAND KharvoxAerSourceTrackingTests)
  add_executable(KharvoxAerWorldViewHistoryTests tests/aer_world_view_history_tests.cpp)
  target_compile_features(KharvoxAerWorldViewHistoryTests PRIVATE cxx_std_17)
  add_test(NAME aer-world-view-history COMMAND KharvoxAerWorldViewHistoryTests)
  add_executable(KharvoxAerCameraPairCacheTests tests/aer_camera_pair_cache_tests.cpp)
  target_compile_features(KharvoxAerCameraPairCacheTests PRIVATE cxx_std_17)
  add_test(NAME aer-camera-pair-cache COMMAND KharvoxAerCameraPairCacheTests)
  add_executable(KharvoxFreshAerHandsPolicyTests tests/fresh_aer_hands_policy_tests.cpp)
  target_compile_features(KharvoxFreshAerHandsPolicyTests PRIVATE cxx_std_17)
  add_test(NAME fresh-aer-hands-policy COMMAND KharvoxFreshAerHandsPolicyTests)
  add_executable(KharvoxAerWeaponPoseCacheTests tests/aer_weapon_pose_cache_tests.cpp)
  target_compile_features(KharvoxAerWeaponPoseCacheTests PRIVATE cxx_std_17)
  add_test(NAME aer-weapon-pose-cache COMMAND KharvoxAerWeaponPoseCacheTests)
  add_executable(KharvoxEngineMemoryCapacityTests tests/engine_memory_capacity_tests.cpp)
  target_compile_features(KharvoxEngineMemoryCapacityTests PRIVATE cxx_std_17)
  add_test(NAME engine-memory-capacity COMMAND KharvoxEngineMemoryCapacityTests)
  add_executable(KharvoxIndependentSurfaceTests tests/independent_surface_tests.cpp)
  target_compile_features(KharvoxIndependentSurfaceTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxIndependentSurfaceTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME independent-surface COMMAND KharvoxIndependentSurfaceTests)
  if(WIN32)
    add_executable(KharvoxCoreSurfaceSmoke tests/core_surface_smoke.cpp)
    target_compile_features(KharvoxCoreSurfaceSmoke PRIVATE cxx_std_17)
    target_include_directories(KharvoxCoreSurfaceSmoke PRIVATE third-party/vulkan-headers/include)
    target_link_libraries(KharvoxCoreSurfaceSmoke PRIVATE user32)
    add_executable(KharvoxIndependentSurfaceSmoke tests/independent_surface_smoke.cpp)
    target_compile_features(KharvoxIndependentSurfaceSmoke PRIVATE cxx_std_17)
    target_include_directories(KharvoxIndependentSurfaceSmoke PRIVATE third-party/vulkan-headers/include)
    target_link_libraries(KharvoxIndependentSurfaceSmoke PRIVATE user32)
  endif()

endif()

option(KHARVOX_BUILD_LAYER "Build the KHARVOX Vulkan compatibility layer" ON)
set(KHARVOX_PSVR2_TOOLKIT_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/PSVR2Toolkit" CACHE PATH
  "Path to a PSVR2 Toolkit source checkout")
set(KHARVOX_PSVR2_CAPI_DIR
  "${KHARVOX_PSVR2_TOOLKIT_SOURCE_DIR}/projects/psvr2_toolkit_capi")
set(KHARVOX_PSVR2_COMMON_DIR
  "${KHARVOX_PSVR2_TOOLKIT_SOURCE_DIR}/projects/common")
if(EXISTS "${KHARVOX_PSVR2_CAPI_DIR}/psvr2tk_capi.h"
    AND EXISTS "${KHARVOX_PSVR2_COMMON_DIR}/common.h")
  set(KHARVOX_HAVE_PSVR2_TOOLKIT ON)
else()
  set(KHARVOX_HAVE_PSVR2_TOOLKIT OFF)
  message(STATUS
    "PSVR2 Toolkit headers not found; KharvoxPsvr2Bridge will not be built")
endif()

if(KHARVOX_BUILD_LAYER)
  if(BUILD_TESTING AND WIN32)
    add_executable(KharvoxForeignProcessLayerTests tests/foreign_process_layer_tests.cpp)
    add_dependencies(KharvoxForeignProcessLayerTests KharvoxLayer)
    target_compile_features(KharvoxForeignProcessLayerTests PRIVATE cxx_std_17)
    target_include_directories(KharvoxForeignProcessLayerTests PRIVATE third-party/vulkan-headers/include)
    add_test(NAME foreign-process-layer COMMAND KharvoxForeignProcessLayerTests $<TARGET_FILE:KharvoxLayer>)
  endif()
  add_subdirectory(third-party/minhook)
  add_library(KharvoxNativeStereo OBJECT src/native/NativeStereo.cpp src/native/NativeSupport.cpp src/native/NativeFrameTrace.cpp src/native/upstream/src/xr/wraparound.cpp)
  target_compile_features(KharvoxNativeStereo PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(KharvoxNativeStereo PRIVATE /bigobj /we4715)
  endif()
  target_compile_definitions(KharvoxNativeStereo PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR DOOMVR_RESEARCH_TOOLS=1)
  target_include_directories(KharvoxNativeStereo PRIVATE src/native/upstream/include third-party/minhook/include third-party/vulkan-headers/include third-party/openxr-sdk/include)
  add_library(KharvoxLayer SHARED $<TARGET_OBJECTS:KharvoxNativeStereo> src/vulkan/KharvoxLayer.cpp src/openxr/OpenXRBootstrap.cpp src/bhaptics/BhapticsIpcClient.cpp src/psvr2/Psvr2IpcClient.cpp src/fsr/Fsr1Upscaler.cpp src/hands/HandRenderer.cpp src/hands/HandSceneDepthTracker.cpp src/camera/CameraHook.cpp src/hud/HudHook.cpp src/hud/HudGpuDiagnostic.cpp src/weapon/WeaponHook.cpp src/debug/CameraScanner.cpp src/vulkan/KharvoxLayer.def src/common/Version.rc)
  target_compile_features(KharvoxLayer PRIVATE cxx_std_17)
  if(KHARVOX_BUILD_SFS_COMPILER)
    target_sources(KharvoxLayer PRIVATE src/sfs/NativeSfs.cpp)
    target_compile_definitions(KharvoxNativeStereo PRIVATE KHARVOX_HAVE_SFS_COMPILER)
    target_link_libraries(KharvoxLayer PRIVATE KharvoxSfsCompiler)
    target_compile_definitions(KharvoxLayer PRIVATE KHARVOX_HAVE_SFS_COMPILER)
    add_custom_command(TARGET KharvoxLayer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/third-party/sfs-compiler-licenses"
        "$<TARGET_FILE_DIR:KharvoxLayer>/sfs-compiler-licenses"
      COMMAND ${CMAKE_COMMAND}
        "-DLAYER=$<TARGET_FILE:KharvoxLayer>"
        "-DOUTPUT=$<TARGET_FILE_DIR:KharvoxLayer>/native_sfs_build.txt"
        -P "${PROJECT_SOURCE_DIR}/tools/write_sfs_capability.cmake"
      VERBATIM)
  else()
    add_custom_command(TARGET KharvoxLayer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:KharvoxLayer>/native_sfs_build.txt"
      VERBATIM)
  endif()
  if(MSVC)
    target_compile_options(KharvoxNativeStereo PRIVATE "$<$<CONFIG:Release>:/Zi>")
    target_compile_options(KharvoxLayer PRIVATE "$<$<CONFIG:Release>:/Zi>")
    target_link_options(KharvoxLayer PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
  endif()
  target_compile_definitions(KharvoxLayer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_USE_PLATFORM_WIN32_KHR)
  target_include_directories(KharvoxLayer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/vulkan-headers/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/openxr-sdk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/cgltf")
  if(WIN32)
    target_link_libraries(KharvoxLayer PRIVATE bcrypt minhook user32 gdi32 advapi32 ole32 windowscodecs)
  endif()
  set_target_properties(KharvoxLayer PROPERTIES
    OUTPUT_NAME "KharvoxLayer"
    VERSION "${PROJECT_VERSION}")
  add_custom_command(TARGET KharvoxLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/offhand_hud_default.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/offhand_hud_default.cfg")
  # Calibration-only changes must refresh shipped defaults even when no C++
  # source changed (the post-build copy otherwise need not run).
  set_property(TARGET KharvoxLayer APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/hand_models_calibration_default.cfg"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/offhand_hud_default.cfg"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/hud_flat_calibration_default.cfg"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/hud_profile_calibration_default.cfg"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/hud_quad_scale_default.cfg")
  add_custom_command(TARGET KharvoxLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/KharvoxLayer.json"
      "$<TARGET_FILE_DIR:KharvoxLayer>/KharvoxLayer.json")
  add_custom_command(TARGET KharvoxLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/src/fsr/Fsr1Easu.spv"
      "$<TARGET_FILE_DIR:KharvoxLayer>/Fsr1Easu.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/src/fsr/Fsr1Rcas.spv"
      "$<TARGET_FILE_DIR:KharvoxLayer>/Fsr1Rcas.spv")
  add_custom_command(TARGET KharvoxLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/src/hands/HandPbr.vert.spv"
      "$<TARGET_FILE_DIR:KharvoxLayer>/HandPbr.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/src/hands/HandPbr.frag.spv"
      "$<TARGET_FILE_DIR:KharvoxLayer>/HandPbr.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/hand_models.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/hand_models.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/hand_models_calibration_default.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/hand_models_calibration_default.cfg"
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:KharvoxLayer>/assets/3D"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/3D/DOOM_LEFT_HAND_FIST.glb"
      "$<TARGET_FILE_DIR:KharvoxLayer>/assets/3D/DOOM_LEFT_HAND_FIST.glb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/3D/DOOM_LEFT_HAND_GUN.glb"
      "$<TARGET_FILE_DIR:KharvoxLayer>/assets/3D/DOOM_LEFT_HAND_GUN.glb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/3D/DOOM_RIGHT_HAND_FIST.glb"
      "$<TARGET_FILE_DIR:KharvoxLayer>/assets/3D/DOOM_RIGHT_HAND_FIST.glb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/3D/DOOM_RIGHT_HAND_GUN.glb"
      "$<TARGET_FILE_DIR:KharvoxLayer>/assets/3D/DOOM_RIGHT_HAND_GUN.glb")
  add_custom_command(TARGET KharvoxLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/third-party/openxr-sdk/bin/x64/openxr_loader.dll"
      "$<TARGET_FILE_DIR:KharvoxLayer>/openxr_loader.dll")
  add_custom_command(TARGET KharvoxLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/hud_flat_calibration_default.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/hud_flat_calibration_default.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/hud_profile_calibration_default.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/hud_profile_calibration_default.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/hud_quad_scale_default.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/hud_quad_scale_default.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/weapon_yaw_default.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/weapon_yaw_default.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/weapon_roll_default.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/weapon_roll_default.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/weapon_pitch_default.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/weapon_pitch_default.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/config/two_hand_weapon_profiles_default.cfg"
      "$<TARGET_FILE_DIR:KharvoxLayer>/two_hand_weapon_profiles_default.cfg")
endif()

if(WIN32)
  add_executable(KharvoxBhapticsBridge
    src/bhaptics/BhapticsBridgeMain.cpp
    src/bhaptics/BhapticsSdkBackend.cpp)
  target_compile_features(KharvoxBhapticsBridge PRIVATE cxx_std_17)
  target_compile_definitions(KharvoxBhapticsBridge PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(KharvoxBhapticsBridge PRIVATE advapi32)
  set_target_properties(KharvoxBhapticsBridge PROPERTIES
    OUTPUT_NAME "KharvoxBhapticsBridge")

  set(KHARVOX_BHAPTICS_SDK_DLL "$ENV{LOCALAPPDATA}/KHARVOX/bhaptics/bhaptics_library.dll"
    CACHE FILEPATH "Approved bHaptics SDK2 DLL to include in release output")
  if(EXISTS "${KHARVOX_BHAPTICS_SDK_DLL}")
    add_custom_command(TARGET KharvoxBhapticsBridge POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KHARVOX_BHAPTICS_SDK_DLL}"
        "$<TARGET_FILE_DIR:KharvoxBhapticsBridge>/bhaptics_library.dll")
  else()
    message(WARNING "bHaptics SDK DLL unavailable; release packaging will fail until KHARVOX_BHAPTICS_SDK_DLL is supplied")
  endif()

  if(KHARVOX_HAVE_PSVR2_TOOLKIT)
    add_executable(KharvoxPsvr2Bridge
      src/psvr2/Psvr2BridgeMain.cpp
      src/psvr2/Psvr2ToolkitBackend.cpp)
    target_compile_features(KharvoxPsvr2Bridge PRIVATE cxx_std_17)
    target_compile_definitions(KharvoxPsvr2Bridge PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(KharvoxPsvr2Bridge PRIVATE
      "${KHARVOX_PSVR2_CAPI_DIR}"
      "${KHARVOX_PSVR2_COMMON_DIR}")
    target_link_libraries(KharvoxPsvr2Bridge PRIVATE advapi32)
    set_target_properties(KharvoxPsvr2Bridge PROPERTIES
      OUTPUT_NAME "KharvoxPsvr2Bridge")
    add_custom_command(TARGET KharvoxPsvr2Bridge POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third-party/psvr2-toolkit/psvr2_toolkit_capi_loader.dll"
        "$<TARGET_FILE_DIR:KharvoxPsvr2Bridge>/psvr2_toolkit_capi_loader.dll")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(KharvoxRuntimePolicyTests tests/runtime_policy_tests.cpp)
  target_compile_features(KharvoxRuntimePolicyTests PRIVATE cxx_std_17)
  target_compile_definitions(KharvoxRuntimePolicyTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(KharvoxRuntimePolicyTests PRIVATE advapi32)
  add_test(NAME runtime-policy COMMAND KharvoxRuntimePolicyTests)

  add_executable(KharvoxRuntimePathsTests tests/runtime_paths_tests.cpp)
  add_executable(KharvoxEquipmentGripPolicyTests tests/equipment_grip_policy_tests.cpp)
  target_compile_features(KharvoxEquipmentGripPolicyTests PRIVATE cxx_std_17)
  add_test(NAME equipment-grip-policy COMMAND KharvoxEquipmentGripPolicyTests)
  target_compile_features(KharvoxRuntimePathsTests PRIVATE cxx_std_17)
  target_compile_definitions(KharvoxRuntimePathsTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(NAME runtime-paths COMMAND KharvoxRuntimePathsTests)

  add_executable(KharvoxDefaultConfigsTests tests/default_configs_tests.cpp)
  target_compile_features(KharvoxDefaultConfigsTests PRIVATE cxx_std_17)
  add_test(NAME default-configs COMMAND KharvoxDefaultConfigsTests "${CMAKE_CURRENT_SOURCE_DIR}/config")

  add_executable(KharvoxHudLayoutPolicyTests tests/hud_layout_policy_tests.cpp)
  target_compile_features(KharvoxHudLayoutPolicyTests PRIVATE cxx_std_17)
  add_test(NAME hud-layout-policy COMMAND KharvoxHudLayoutPolicyTests)

  add_executable(KharvoxHudMenuPolicyTests tests/hud_menu_policy_tests.cpp)
  target_compile_features(KharvoxHudMenuPolicyTests PRIVATE cxx_std_17)
  add_test(NAME hud-menu-policy COMMAND KharvoxHudMenuPolicyTests)

  add_executable(KharvoxCameraBasisPolicyTests tests/camera_basis_policy_tests.cpp)
  target_compile_features(KharvoxCameraBasisPolicyTests PRIVATE cxx_std_17)
  add_test(NAME camera-basis-policy COMMAND KharvoxCameraBasisPolicyTests)
  add_executable(KharvoxBodyStanceHeightTests tests/body_stance_height_tests.cpp)
  target_compile_features(KharvoxBodyStanceHeightTests PRIVATE cxx_std_17)
  add_test(NAME body-stance-height COMMAND KharvoxBodyStanceHeightTests)
  add_executable(KharvoxTutorialRenderTests tests/tutorial_render_tests.cpp)
  target_compile_features(KharvoxTutorialRenderTests PRIVATE cxx_std_17)
  add_test(NAME tutorial-render COMMAND KharvoxTutorialRenderTests)

  add_executable(KharvoxMovementDirectionPolicyTests
    tests/movement_direction_policy_tests.cpp)
  target_compile_features(KharvoxMovementDirectionPolicyTests PRIVATE cxx_std_17)
  add_test(NAME movement-direction-policy COMMAND KharvoxMovementDirectionPolicyTests)

  add_executable(KharvoxGripThresholdPolicyTests
    tests/grip_threshold_policy_tests.cpp)
  target_compile_features(KharvoxGripThresholdPolicyTests PRIVATE cxx_std_17)
  add_test(NAME grip-threshold-policy COMMAND KharvoxGripThresholdPolicyTests)

  add_executable(KharvoxSnapTurnStereoPolicyTests
    tests/snap_turn_stereo_policy_tests.cpp)
  target_compile_features(KharvoxSnapTurnStereoPolicyTests PRIVATE cxx_std_17)
  add_test(NAME snap-turn-stereo-policy COMMAND KharvoxSnapTurnStereoPolicyTests)

  add_executable(KharvoxPostCinematicYawPolicyTests tests/post_cinematic_yaw_policy_tests.cpp)
  target_compile_features(KharvoxPostCinematicYawPolicyTests PRIVATE cxx_std_17)
  add_test(NAME post-cinematic-yaw-policy COMMAND KharvoxPostCinematicYawPolicyTests)

  add_executable(KharvoxHandsJumpPolicyTests tests/hands_jump_policy_tests.cpp)
  add_test(NAME hands-jump-policy COMMAND KharvoxHandsJumpPolicyTests)
  add_executable(KharvoxCinewindowPosePolicyTests tests/cinewindow_pose_policy_tests.cpp)
  target_compile_features(KharvoxCinewindowPosePolicyTests PRIVATE cxx_std_17)
  add_test(NAME cinewindow-pose-policy COMMAND KharvoxCinewindowPosePolicyTests)

  add_executable(KharvoxXInputHapticsPolicyTests tests/xinput_haptics_policy_tests.cpp)
  target_compile_features(KharvoxXInputHapticsPolicyTests PRIVATE cxx_std_17)
  add_test(NAME xinput-haptics-policy COMMAND KharvoxXInputHapticsPolicyTests)

  add_executable(KharvoxBackWeaponPolicyTests tests/back_weapon_policy_tests.cpp)
  target_compile_features(KharvoxBackWeaponPolicyTests PRIVATE cxx_std_17)
  add_test(NAME back-weapon-policy COMMAND KharvoxBackWeaponPolicyTests)

  add_executable(KharvoxMotionWeaponWheelPolicyTests tests/motion_weapon_wheel_policy_tests.cpp)
  target_compile_features(KharvoxMotionWeaponWheelPolicyTests PRIVATE cxx_std_17)
  add_test(NAME motion-weapon-wheel-policy COMMAND KharvoxMotionWeaponWheelPolicyTests)

  add_executable(KharvoxAerWeaponPairPolicyTests
    tests/aer_weapon_pair_policy_tests.cpp)
  target_compile_features(KharvoxAerWeaponPairPolicyTests PRIVATE cxx_std_17)
  add_test(NAME aer-weapon-pair-policy COMMAND KharvoxAerWeaponPairPolicyTests)

  add_executable(KharvoxFsr1PolicyTests tests/fsr1_policy_tests.cpp)
  target_compile_features(KharvoxFsr1PolicyTests PRIVATE cxx_std_17)
  add_test(NAME fsr1-policy COMMAND KharvoxFsr1PolicyTests)

  add_executable(KharvoxHandVisibilityPolicyTests
    tests/hand_visibility_policy_tests.cpp)
  target_compile_features(KharvoxHandVisibilityPolicyTests PRIVATE cxx_std_17)
  add_test(NAME hand-visibility-policy COMMAND KharvoxHandVisibilityPolicyTests)

  add_executable(KharvoxHandDepthPolicyTests
    tests/hand_depth_policy_tests.cpp)
  target_compile_features(KharvoxHandDepthPolicyTests PRIVATE cxx_std_17)
  add_test(NAME hand-depth-policy COMMAND KharvoxHandDepthPolicyTests)

  add_executable(KharvoxHandCalibrationPolicyTests
    tests/hand_calibration_policy_tests.cpp)
  target_compile_features(KharvoxHandCalibrationPolicyTests PRIVATE cxx_std_17)
  add_test(NAME hand-calibration-policy COMMAND KharvoxHandCalibrationPolicyTests)

  add_executable(KharvoxHandAssetTests tests/hand_asset_tests.cpp)
  target_compile_features(KharvoxHandAssetTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxHandAssetTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/cgltf")
  add_test(NAME hand-assets COMMAND KharvoxHandAssetTests
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/3D")

  add_executable(KharvoxBhapticsProtocolTests tests/bhaptics_protocol_tests.cpp)
  target_compile_features(KharvoxBhapticsProtocolTests PRIVATE cxx_std_17)
  add_test(NAME bhaptics-protocol COMMAND KharvoxBhapticsProtocolTests)

  add_executable(KharvoxBhapticsMappingPolicyTests tests/bhaptics_mapping_policy_tests.cpp)
  target_compile_features(KharvoxBhapticsMappingPolicyTests PRIVATE cxx_std_17)
  add_test(NAME bhaptics-mapping-policy COMMAND KharvoxBhapticsMappingPolicyTests)

  add_executable(KharvoxPsvr2TriggerPolicyTests
    tests/psvr2_trigger_policy_tests.cpp)
  target_compile_features(KharvoxPsvr2TriggerPolicyTests PRIVATE cxx_std_17)
  add_test(NAME psvr2-trigger-policy COMMAND KharvoxPsvr2TriggerPolicyTests)

  add_executable(KharvoxPsvr2ProtocolTests tests/psvr2_protocol_tests.cpp)
  target_compile_features(KharvoxPsvr2ProtocolTests PRIVATE cxx_std_17)
  add_test(NAME psvr2-protocol COMMAND KharvoxPsvr2ProtocolTests)

  if(WIN32)
    add_library(KharvoxBhapticsIncompleteSdk SHARED tests/bhaptics_incomplete_sdk.cpp)
    target_compile_features(KharvoxBhapticsIncompleteSdk PRIVATE cxx_std_17)
    set_target_properties(KharvoxBhapticsIncompleteSdk PROPERTIES
      OUTPUT_NAME "bhaptics_library"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/incomplete-bhaptics-sdk")

    add_executable(KharvoxBhapticsBackendTests
      tests/bhaptics_backend_tests.cpp
      src/bhaptics/BhapticsSdkBackend.cpp)
    target_compile_features(KharvoxBhapticsBackendTests PRIVATE cxx_std_17)
    target_compile_definitions(KharvoxBhapticsBackendTests PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(KharvoxBhapticsBackendTests PRIVATE advapi32)
    add_dependencies(KharvoxBhapticsBackendTests KharvoxBhapticsIncompleteSdk)
    add_test(NAME bhaptics-backend COMMAND KharvoxBhapticsBackendTests
      "$<TARGET_FILE_DIR:KharvoxBhapticsIncompleteSdk>")

    add_executable(KharvoxBhapticsPipeIntegrationTests
      tests/bhaptics_pipe_integration_tests.cpp)
    target_compile_features(KharvoxBhapticsPipeIntegrationTests PRIVATE cxx_std_17)
    target_compile_definitions(KharvoxBhapticsPipeIntegrationTests PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    add_dependencies(KharvoxBhapticsPipeIntegrationTests
      KharvoxBhapticsBridge KharvoxBhapticsIncompleteSdk)
    add_test(NAME bhaptics-pipe-integration
      COMMAND KharvoxBhapticsPipeIntegrationTests
        "$<TARGET_FILE:KharvoxBhapticsBridge>"
        "$<TARGET_FILE:KharvoxBhapticsIncompleteSdk>")

    if(KHARVOX_HAVE_PSVR2_TOOLKIT)
    add_library(KharvoxPsvr2FakeLoader SHARED
      tests/psvr2_fake_loader.cpp)
    target_compile_features(KharvoxPsvr2FakeLoader PRIVATE cxx_std_17)
    target_compile_definitions(KharvoxPsvr2FakeLoader PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(KharvoxPsvr2FakeLoader PRIVATE
      "${KHARVOX_PSVR2_CAPI_DIR}"
      "${KHARVOX_PSVR2_COMMON_DIR}")
    set_target_properties(KharvoxPsvr2FakeLoader PROPERTIES
      OUTPUT_NAME "psvr2_toolkit_capi_loader"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-psvr2-loader"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-psvr2-loader")

    add_library(KharvoxPsvr2FakeIncompleteLoader SHARED
      tests/psvr2_fake_incomplete_loader.cpp)
    target_compile_features(KharvoxPsvr2FakeIncompleteLoader PRIVATE cxx_std_17)
    target_compile_definitions(KharvoxPsvr2FakeIncompleteLoader PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    set_target_properties(KharvoxPsvr2FakeIncompleteLoader PROPERTIES
      OUTPUT_NAME "psvr2_toolkit_capi_loader"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-psvr2-incomplete-loader"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-psvr2-incomplete-loader")

    add_executable(KharvoxPsvr2BackendTests
      tests/psvr2_backend_tests.cpp
      src/psvr2/Psvr2ToolkitBackend.cpp)
    target_compile_features(KharvoxPsvr2BackendTests PRIVATE cxx_std_17)
    target_compile_definitions(KharvoxPsvr2BackendTests PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(KharvoxPsvr2BackendTests PRIVATE
      "${KHARVOX_PSVR2_CAPI_DIR}"
      "${KHARVOX_PSVR2_COMMON_DIR}")
    add_dependencies(KharvoxPsvr2BackendTests
      KharvoxPsvr2FakeLoader KharvoxPsvr2FakeIncompleteLoader)
    add_test(NAME psvr2-backend COMMAND KharvoxPsvr2BackendTests
      "$<TARGET_FILE_DIR:KharvoxPsvr2FakeLoader>"
      "$<TARGET_FILE_DIR:KharvoxPsvr2FakeIncompleteLoader>"
      "${CMAKE_CURRENT_BINARY_DIR}/missing-psvr2-loader")
    endif()
  endif()
endif()

if(BUILD_TESTING)
 if(WIN32)
  add_executable(KharvoxNativeMappedCopyTests tests/native_mapped_copy_tests.cpp)

  add_executable(KharvoxNativeLoadingDrainTests tests/native_loading_drain_tests.cpp)
  add_executable(KharvoxNativeInlineStartupTests tests/native_inline_startup_tests.cpp)
  add_executable(KharvoxBodyPoseRebaseTests tests/body_pose_rebase_tests.cpp)
  target_compile_features(KharvoxBodyPoseRebaseTests PRIVATE cxx_std_17)
  add_test(NAME body-pose-rebase COMMAND KharvoxBodyPoseRebaseTests)
  add_executable(KharvoxPoseTraceTests tests/pose_trace_tests.cpp)
  target_compile_features(KharvoxPoseTraceTests PRIVATE cxx_std_17)
  add_test(NAME pose-trace COMMAND KharvoxPoseTraceTests)
  add_executable(KharvoxNativeStartupControlsTests tests/native_startup_controls_tests.cpp)
  target_compile_features(KharvoxNativeStartupControlsTests PRIVATE cxx_std_17)
  add_test(NAME native-startup-controls COMMAND KharvoxNativeStartupControlsTests)
  add_executable(KharvoxNativeQualityTransitionTests tests/native_quality_transition_tests.cpp)
  target_compile_features(KharvoxNativeQualityTransitionTests PRIVATE cxx_std_17)
  add_test(NAME native-quality-transition COMMAND KharvoxNativeQualityTransitionTests)
  target_compile_features(KharvoxNativeInlineStartupTests PRIVATE cxx_std_17)
  add_test(NAME native-inline-startup COMMAND KharvoxNativeInlineStartupTests)
  target_compile_features(KharvoxNativeLoadingDrainTests PRIVATE cxx_std_17)
  add_test(NAME native-loading-drain COMMAND KharvoxNativeLoadingDrainTests)
  target_compile_features(KharvoxNativeMappedCopyTests PRIVATE cxx_std_17)
  add_test(NAME native-mapped-copy COMMAND KharvoxNativeMappedCopyTests)
  add_executable(KharvoxNativeCaptureTriggerTests tests/native_capture_trigger_tests.cpp)
  target_compile_features(KharvoxNativeCaptureTriggerTests PRIVATE cxx_std_17)
  add_test(NAME native-capture-trigger COMMAND KharvoxNativeCaptureTriggerTests)
  add_executable(KharvoxNativeEyeBindingsTests tests/native_eye_bindings_tests.cpp)
  target_compile_features(KharvoxNativeEyeBindingsTests PRIVATE cxx_std_17)
  add_test(NAME native-eye-bindings COMMAND KharvoxNativeEyeBindingsTests)
  add_executable(KharvoxNativePairWatchTests tests/native_pair_watch_tests.cpp)
  target_compile_features(KharvoxNativePairWatchTests PRIVATE cxx_std_17)
  add_test(NAME native-pair-watch COMMAND KharvoxNativePairWatchTests)
  add_executable(KharvoxNativeDescriptorReuseTests tests/native_descriptor_reuse_tests.cpp)
  target_compile_features(KharvoxNativeDescriptorReuseTests PRIVATE cxx_std_17)
  add_test(NAME native-descriptor-reuse COMMAND KharvoxNativeDescriptorReuseTests)
  add_executable(KharvoxNativeDescriptorLifetimeTests tests/native_descriptor_lifetime_tests.cpp)
  target_compile_features(KharvoxNativeDescriptorLifetimeTests PRIVATE cxx_std_17)
  add_test(NAME native-descriptor-lifetime COMMAND KharvoxNativeDescriptorLifetimeTests)
  add_executable(KharvoxNativeDescriptorArenaTests tests/native_descriptor_arena_tests.cpp)
  target_compile_features(KharvoxNativeDescriptorArenaTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxNativeDescriptorArenaTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME native-descriptor-arena COMMAND KharvoxNativeDescriptorArenaTests)
  add_executable(KharvoxNativeResourceSubsetTests tests/native_resource_subset_tests.cpp)
  target_compile_features(KharvoxNativeResourceSubsetTests PRIVATE cxx_std_17)
  add_test(NAME native-resource-subset COMMAND KharvoxNativeResourceSubsetTests)
  add_executable(KharvoxNativeShadowHistoryTests tests/native_shadow_history_tests.cpp)
  target_compile_features(KharvoxNativeShadowHistoryTests PRIVATE cxx_std_17)
  add_test(NAME native-shadow-history COMMAND KharvoxNativeShadowHistoryTests)
  add_executable(KharvoxNativeGpuTimingTests tests/native_gpu_timing_tests.cpp)
  target_compile_features(KharvoxNativeGpuTimingTests PRIVATE cxx_std_17)
  add_test(NAME native-gpu-timing COMMAND KharvoxNativeGpuTimingTests)
  add_executable(KharvoxNativeSnapshotMemoryTests tests/native_snapshot_memory_tests.cpp)
  target_compile_features(KharvoxNativeSnapshotMemoryTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxNativeSnapshotMemoryTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME native-snapshot-memory COMMAND KharvoxNativeSnapshotMemoryTests)
  # Run explicitly on a test machine; intentionally absent from CTest.
  add_executable(KharvoxNativeMappedCopyBench tools/native_mapped_copy_bench.cpp)
  target_compile_features(KharvoxNativeMappedCopyBench PRIVATE cxx_std_17)
  target_include_directories(KharvoxNativeMappedCopyBench PRIVATE third-party/vulkan-headers/include)
 endif()
 add_executable(KharvoxRuntimeVulkanDispatchTests tests/runtime_vulkan_dispatch_tests.cpp)
 target_compile_features(KharvoxRuntimeVulkanDispatchTests PRIVATE cxx_std_17)
 target_include_directories(KharvoxRuntimeVulkanDispatchTests PRIVATE third-party/vulkan-headers/include)
 add_test(NAME runtime-vulkan-dispatch COMMAND KharvoxRuntimeVulkanDispatchTests)
 add_executable(KharvoxGameImageLifetimeTests tests/game_image_lifetime_tests.cpp)
 add_executable(KharvoxHandSceneDepthTrackerTests tests/hand_scene_depth_tracker_tests.cpp src/hands/HandSceneDepthTracker.cpp)
 target_compile_features(KharvoxHandSceneDepthTrackerTests PRIVATE cxx_std_17)
 target_compile_definitions(KharvoxHandSceneDepthTrackerTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
 target_include_directories(KharvoxHandSceneDepthTrackerTests PRIVATE third-party/vulkan-headers/include)
 add_test(NAME hand-scene-depth-tracker COMMAND KharvoxHandSceneDepthTrackerTests)
 target_compile_features(KharvoxGameImageLifetimeTests PRIVATE cxx_std_17)
 target_include_directories(KharvoxGameImageLifetimeTests PRIVATE third-party/vulkan-headers/include)
 add_test(NAME game-image-lifetime COMMAND KharvoxGameImageLifetimeTests)
 set_tests_properties(game-image-lifetime PROPERTIES TIMEOUT 10)
 add_executable(KharvoxNativeStereoPolicyTests tests/native_stereo_policy_tests.cpp)
 add_executable(KharvoxNativeQueueCompletionTests tests/native_queue_completion_tests.cpp)
 target_compile_features(KharvoxNativeQueueCompletionTests PRIVATE cxx_std_17)
 target_include_directories(KharvoxNativeQueueCompletionTests PRIVATE third-party/vulkan-headers/include)
 add_test(NAME native-queue-completion COMMAND KharvoxNativeQueueCompletionTests)
 add_executable(KharvoxNativeCommandLifetimeTests tests/native_command_lifetime_tests.cpp)
 target_compile_features(KharvoxNativeCommandLifetimeTests PRIVATE cxx_std_17)
 target_include_directories(KharvoxNativeCommandLifetimeTests PRIVATE third-party/vulkan-headers/include)
 add_test(NAME native-command-lifetime COMMAND KharvoxNativeCommandLifetimeTests)
 add_executable(KharvoxNativePassInputIndexTests tests/native_pass_input_index_tests.cpp)
 add_executable(KharvoxDispatchMemberLookupTests tests/dispatch_member_lookup_tests.cpp)
 target_compile_features(KharvoxDispatchMemberLookupTests PRIVATE cxx_std_17)
 add_test(NAME dispatch-member-lookup COMMAND KharvoxDispatchMemberLookupTests)
 target_compile_features(KharvoxNativePassInputIndexTests PRIVATE cxx_std_17)
 add_test(NAME native-pass-input-index COMMAND KharvoxNativePassInputIndexTests)
 add_executable(KharvoxNativeLightCullingTests tests/native_light_culling_tests.cpp)
 add_executable(KharvoxNativeBindScratchTests tests/native_bind_scratch_tests.cpp)
 add_executable(KharvoxNativePacingSequenceTests tests/native_pacing_sequence_tests.cpp)
 target_compile_features(KharvoxNativePacingSequenceTests PRIVATE cxx_std_17)
  add_test(NAME native-pacing-sequence COMMAND KharvoxNativePacingSequenceTests)
  add_executable(KharvoxNativeDeferredMemoryTests tests/native_deferred_memory_tests.cpp)
  target_compile_features(KharvoxNativeDeferredMemoryTests PRIVATE cxx_std_17)
  add_test(NAME native-deferred-memory COMMAND KharvoxNativeDeferredMemoryTests)
  add_executable(KharvoxNativeResourceRetirementTests tests/native_resource_retirement_tests.cpp)
  target_compile_features(KharvoxNativeResourceRetirementTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxNativeResourceRetirementTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME native-resource-retirement COMMAND KharvoxNativeResourceRetirementTests)
  add_executable(KharvoxNativeStorageMirrorBudgetTests tests/native_storage_mirror_budget_tests.cpp)
  target_compile_features(KharvoxNativeStorageMirrorBudgetTests PRIVATE cxx_std_17)
  add_test(NAME native-storage-mirror-budget COMMAND KharvoxNativeStorageMirrorBudgetTests)
  add_executable(KharvoxNativeXrReleaseTests tests/native_xr_release_tests.cpp)
  target_compile_features(KharvoxNativeXrReleaseTests PRIVATE cxx_std_17)
  target_compile_definitions(KharvoxNativeXrReleaseTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(NAME native-xr-release COMMAND KharvoxNativeXrReleaseTests)
 if(WIN32)
  add_executable(KharvoxNativeResourceGuardTests tests/native_resource_guard_tests.cpp)
  target_compile_features(KharvoxNativeResourceGuardTests PRIVATE cxx_std_20)
  add_test(NAME native-resource-guard COMMAND KharvoxNativeResourceGuardTests)
 endif()
 add_executable(KharvoxNativeBindingMemoTests tests/native_binding_memo_tests.cpp)
 add_executable(KharvoxNativeSnapshotAccessClassesTests tests/native_snapshot_access_classes_tests.cpp)
 target_compile_features(KharvoxNativeSnapshotAccessClassesTests PRIVATE cxx_std_17)
 add_test(NAME native-snapshot-access-classes COMMAND KharvoxNativeSnapshotAccessClassesTests)
 target_compile_features(KharvoxNativeBindingMemoTests PRIVATE cxx_std_17)
 add_test(NAME native-binding-memo COMMAND KharvoxNativeBindingMemoTests)
 target_compile_features(KharvoxNativeBindScratchTests PRIVATE cxx_std_17)
 add_test(NAME native-bind-scratch COMMAND KharvoxNativeBindScratchTests)
 target_compile_features(KharvoxNativeLightCullingTests PRIVATE cxx_std_17)
 add_test(NAME native-light-culling COMMAND KharvoxNativeLightCullingTests)
 target_compile_features(KharvoxNativeStereoPolicyTests PRIVATE cxx_std_17)
 add_test(NAME native-stereo-policy COMMAND KharvoxNativeStereoPolicyTests)
 add_executable(KharvoxNativeImageLayoutTests tests/native_image_layout_tests.cpp)
 target_compile_features(KharvoxNativeImageLayoutTests PRIVATE cxx_std_17)
 target_include_directories(KharvoxNativeImageLayoutTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME native-image-layouts COMMAND KharvoxNativeImageLayoutTests)
  add_executable(KharvoxNativeSourceLayoutTests tests/native_source_layout_tests.cpp)
  target_compile_features(KharvoxNativeSourceLayoutTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxNativeSourceLayoutTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME native-source-layouts COMMAND KharvoxNativeSourceLayoutTests)
  add_executable(KharvoxNativeImagePlanTests tests/native_image_plan_tests.cpp)
  target_compile_features(KharvoxNativeImagePlanTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxNativeImagePlanTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME native-image-plans COMMAND KharvoxNativeImagePlanTests)
  add_executable(KharvoxNativeSnapshotBatchTests tests/native_snapshot_batch_tests.cpp)
  target_compile_features(KharvoxNativeSnapshotBatchTests PRIVATE cxx_std_20)
  add_test(NAME native-snapshot-batch COMMAND KharvoxNativeSnapshotBatchTests)
  add_executable(KharvoxNativeFrameTraceTests tests/native_frame_trace_tests.cpp src/native/NativeFrameTrace.cpp)
  target_compile_features(KharvoxNativeFrameTraceTests PRIVATE cxx_std_20)
  target_compile_definitions(KharvoxNativeFrameTraceTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
  target_include_directories(KharvoxNativeFrameTraceTests PRIVATE third-party/vulkan-headers/include third-party/openxr-sdk/include)
  target_link_libraries(KharvoxNativeFrameTraceTests PRIVATE user32)
  set_target_properties(KharvoxNativeFrameTraceTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/frame-trace-tests")
  add_test(NAME native-frame-trace COMMAND KharvoxNativeFrameTraceTests)
  add_executable(KharvoxNativeSpirvReadOnlyTests tests/native_spirv_readonly_tests.cpp)
  target_compile_features(KharvoxNativeSpirvReadOnlyTests PRIVATE cxx_std_17)
  add_test(NAME native-spirv-readonly COMMAND KharvoxNativeSpirvReadOnlyTests)
 foreach(name wraparound_math_test freqhigh_record_test)
  add_executable(Native_${name} tests/native_${name}.cpp)
  target_include_directories(Native_${name} PRIVATE src/native/upstream/include)
  target_compile_features(Native_${name} PRIVATE cxx_std_20)
  add_test(NAME native-${name} COMMAND Native_${name})
 endforeach()
endif()

if(WIN32 AND BUILD_TESTING)
  add_executable(KharvoxDoomViewEffectsTests tests/doom_view_effects_tests.cpp)
  target_compile_features(KharvoxDoomViewEffectsTests PRIVATE cxx_std_17)
  add_test(NAME doom-view-effects COMMAND KharvoxDoomViewEffectsTests)
  add_executable(KharvoxRuntimeLogTests tests/runtime_log_tests.cpp)
  target_compile_features(KharvoxRuntimeLogTests PRIVATE cxx_std_17)
  add_test(NAME runtime-log-disabled COMMAND KharvoxRuntimeLogTests 0)
  add_test(NAME runtime-log-enabled COMMAND KharvoxRuntimeLogTests 1)
  add_executable(KharvoxEyeCapturePngTests tests/eye_capture_png_tests.cpp)
  target_compile_features(KharvoxEyeCapturePngTests PRIVATE cxx_std_17)
  target_link_libraries(KharvoxEyeCapturePngTests PRIVATE ole32 windowscodecs)
  add_test(NAME eye-capture-png COMMAND KharvoxEyeCapturePngTests)
  add_executable(KharvoxHandSceneDepthGpuTests tests/hand_scene_depth_gpu_tests.cpp)
  target_compile_features(KharvoxHandSceneDepthGpuTests PRIVATE cxx_std_17)
  target_compile_definitions(KharvoxHandSceneDepthGpuTests PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(KharvoxHandSceneDepthGpuTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME hand-scene-depth-gpu COMMAND KharvoxHandSceneDepthGpuTests)
  add_executable(KharvoxNativeDepthSeedGpuTests tests/native_depth_seed_gpu_tests.cpp)
  target_compile_features(KharvoxNativeDepthSeedGpuTests PRIVATE cxx_std_17)
  target_compile_definitions(KharvoxNativeDepthSeedGpuTests PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(KharvoxNativeDepthSeedGpuTests PRIVATE third-party/vulkan-headers/include)
  add_test(NAME native-depth-seed-gpu COMMAND KharvoxNativeDepthSeedGpuTests)
  add_executable(KharvoxFsr1GpuTests tests/fsr1_gpu_tests.cpp src/fsr/Fsr1Upscaler.cpp)
  target_compile_features(KharvoxFsr1GpuTests PRIVATE cxx_std_17)
  target_compile_definitions(KharvoxFsr1GpuTests PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(KharvoxFsr1GpuTests PRIVATE third-party/vulkan-headers/include)
  add_custom_command(TARGET KharvoxFsr1GpuTests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fsr/Fsr1Easu.spv" "${CMAKE_CURRENT_SOURCE_DIR}/src/fsr/Fsr1Rcas.spv" "$<TARGET_FILE_DIR:KharvoxFsr1GpuTests>")
  add_test(NAME fsr1-gpu-current-eyes COMMAND KharvoxFsr1GpuTests)
endif()

# Assertions are test checks in both configurations, not release-only no-ops.
if(BUILD_TESTING AND MSVC)
  get_property(kharvox_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(kharvox_target IN LISTS kharvox_targets)
    if(kharvox_target MATCHES "Tests$|^Native_")
      target_compile_options(${kharvox_target} PRIVATE /UNDEBUG)
    endif()
  endforeach()
endif()

if(WIN32)
  configure_file(src/intro/VrGameIntro.rc.in "${CMAKE_CURRENT_BINARY_DIR}/VrGameIntro.rc" @ONLY)
  if(TARGET KharvoxLayer)
    target_sources(KharvoxLayer PRIVATE src/intro/VrGameIntro.cpp src/intro/IntroMusic.cpp src/intro/Pocketmod.c "${CMAKE_CURRENT_BINARY_DIR}/VrGameIntro.rc")
    target_compile_definitions(KharvoxLayer PRIVATE KHARVOX_INTRO_LIBRARY)
    target_link_libraries(KharvoxLayer PRIVATE d3d11 d3dcompiler dxgi shell32 xaudio2)
  endif()
endif()

if(BUILD_TESTING AND WIN32)
  add_executable(KharvoxVrCubeIntroTests tests/vr_cube_intro_tests.cpp "${CMAKE_CURRENT_BINARY_DIR}/VrGameIntro.rc")
  target_compile_features(KharvoxVrCubeIntroTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxVrCubeIntroTests PRIVATE third-party/vulkan-headers/include third-party/openxr-sdk/include)
  target_link_libraries(KharvoxVrCubeIntroTests PRIVATE user32 gdi32 d3d11 d3dcompiler windowscodecs ole32 dxgi)
  add_test(NAME vr-cube-intro COMMAND KharvoxVrCubeIntroTests "${CMAKE_CURRENT_BINARY_DIR}/vr-cube-test")
  add_executable(KharvoxIntroShutdownTests tests/intro_shutdown_tests.cpp src/intro/IntroMusic.cpp src/intro/Pocketmod.c "${CMAKE_CURRENT_BINARY_DIR}/VrGameIntro.rc")
  target_compile_features(KharvoxIntroShutdownTests PRIVATE cxx_std_17)
  target_include_directories(KharvoxIntroShutdownTests PRIVATE third-party/openxr-sdk/include)
  target_link_libraries(KharvoxIntroShutdownTests PRIVATE d3d11 d3dcompiler windowscodecs dxgi user32 gdi32 shell32 xaudio2 ole32)
  add_test(NAME vr-intro-shutdown COMMAND KharvoxIntroShutdownTests)

endif()

if(WIN32)
  add_executable(KharvoxGameIntro WIN32 src/intro/VrGameIntro.cpp src/intro/IntroMusic.cpp src/intro/Pocketmod.c "${CMAKE_CURRENT_BINARY_DIR}/VrGameIntro.rc")
  set_target_properties(KharvoxGameIntro PROPERTIES OUTPUT_NAME "KharvoxIntro")
  target_compile_features(KharvoxGameIntro PRIVATE cxx_std_17)
  target_include_directories(KharvoxGameIntro PRIVATE third-party/openxr-sdk/include)
  target_link_libraries(KharvoxGameIntro PRIVATE d3d11 d3dcompiler windowscodecs dxgi user32 gdi32 shell32 xaudio2 ole32)
  add_custom_command(TARGET KharvoxGameIntro POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/openxr-sdk/bin/x64/openxr_loader.dll"
    "$<TARGET_FILE_DIR:KharvoxGameIntro>/openxr_loader.dll")
endif()
if(BUILD_TESTING AND WIN32)
  add_executable(KharvoxIntroHandshakeStub WIN32 tests/vr_intro_handshake_stub.cpp)
  target_compile_features(KharvoxIntroHandshakeStub PRIVATE cxx_std_17)
  target_link_libraries(KharvoxIntroHandshakeStub PRIVATE shell32)
  set_target_properties(KharvoxIntroHandshakeStub PROPERTIES OUTPUT_NAME KharvoxGameIntro
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/intro-test-helper")
endif()

if(BUILD_TESTING AND WIN32)
  add_executable(KharvoxIntroMusicTests tests/intro_music_tests.cpp src/intro/IntroMusic.cpp src/intro/Pocketmod.c "${CMAKE_CURRENT_BINARY_DIR}/VrGameIntro.rc")
  target_compile_features(KharvoxIntroMusicTests PRIVATE cxx_std_17)
  target_link_libraries(KharvoxIntroMusicTests PRIVATE xaudio2 ole32)
  add_test(NAME vr-intro-music COMMAND KharvoxIntroMusicTests "${CMAKE_CURRENT_BINARY_DIR}/vr-cube-test")
endif()
