# Hairwork Studio (our code: Hairwork Studio License, see LICENSE.md). The NVIDIA HairWorks SDK is NOT part of this repository: its headers are
# included from HAIRWORKS_SDK_DIR at build time (default: <repo>/sources/HairWorks-r1_2_1-15-distro/HairWorks) and its
# runtime DLL is loaded from there (or a user-set path) at run time.
cmake_minimum_required(VERSION 3.20)
project(HairworkStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(HAIRWORKS_SDK_DIR "${REPO_ROOT}/sources/HairWorks-r1_2_1-15-distro/HairWorks" CACHE PATH "NVIDIA HairWorks 1.2.1 SDK folder (include/, bin/win64/)")
if(NOT EXISTS "${HAIRWORKS_SDK_DIR}/include/Nv/HairWorks/NvHairSdk.h")
  message(FATAL_ERROR "HairWorks SDK not found in ${HAIRWORKS_SDK_DIR}. Pass -DHAIRWORKS_SDK_DIR=<folder with include/Nv/HairWorks/NvHairSdk.h>")
endif()

# NVIDIA attribution (GameWorks EULA section 6.B.f): the official GameWorks logo from the SDK is embedded in the exe as
# a resource, so the About box and the splash always show it. No logo = no build. The PNG is never copied into the
# repository (EULA section 8); it only lives in the build folder and inside the binary.
set(GW_LOGO "${HAIRWORKS_SDK_DIR}/docs/guide/_static/developerzone_gameworks_logo.png")
if(NOT EXISTS "${GW_LOGO}")
  message(FATAL_ERROR "NVIDIA GameWorks logo not found (${GW_LOGO}): the attribution logo is required to build.")
endif()
file(TO_CMAKE_PATH "${GW_LOGO}" GW_LOGO_RC)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen/gw_logo.rc" "201 RCDATA \"${GW_LOGO_RC}\"\n")

set(IMGUI third_party/imgui)
add_executable(HairworkStudio WIN32
  src/main.cpp src/app.cpp src/ui.cpp src/agent.cpp src/serve.cpp src/curls.cpp src/gfx.cpp src/hairsdk.cpp src/deskpack.cpp src/groom.cpp src/params.cpp src/motion.cpp src/materials.cpp src/picker.cpp src/stylesw.cpp src/uitest.cpp src/lighting.cpp src/toolbar.cpp src/ui_widgets.cpp src/community.cpp src/community_gh.cpp src/pt_estimate.cpp src/app.rc
  "${CMAKE_CURRENT_BINARY_DIR}/gen/gw_logo.rc"
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/imgui_impl_dx11.cpp ${IMGUI}/imgui_impl_win32.cpp)
target_include_directories(HairworkStudio PRIVATE "${HAIRWORKS_SDK_DIR}/include" ${IMGUI})
file(TO_NATIVE_PATH "${HAIRWORKS_SDK_DIR}" SDK_NATIVE)
file(TO_NATIVE_PATH "${REPO_ROOT}" REPO_NATIVE)
string(REPLACE "\\" "\\\\" SDK_ESC "${SDK_NATIVE}")
string(REPLACE "\\" "\\\\" REPO_ESC "${REPO_NATIVE}")
target_compile_definitions(HairworkStudio PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS IMGUI_ENABLE_TEST_ENGINE
  HW_DEFAULT_SDK_DIR="${SDK_ESC}" HW_REPO_ROOT="${REPO_ESC}")
target_compile_options(HairworkStudio PRIVATE /W3 /MP /utf-8 /Zi)
target_link_options(HairworkStudio PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(HairworkStudio PRIVATE d3d11 dxgi d3dcompiler dxguid dbghelp)

# shaders: compiled at build time with fxc (Windows SDK). hair_ps includes the SDK shader header from HAIRWORKS_SDK_DIR.
find_program(FXC fxc PATHS "$ENV{WindowsSdkVerBinPath}/x64" "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64" REQUIRED)
set(SH ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(OUT $<TARGET_FILE_DIR:HairworkStudio>)
add_custom_command(TARGET HairworkStudio POST_BUILD
  COMMAND ${FXC} /nologo /T ps_5_0 /E main /O3 /I "${HAIRWORKS_SDK_DIR}/include" /I ${SH} /Fo ${OUT}/hair_ps.cso ${SH}/hair_ps.hlsl
  COMMAND ${FXC} /nologo /T vs_5_0 /E vs_main /O3 /Fo ${OUT}/basic_vs.cso ${SH}/basic.hlsl
  COMMAND ${FXC} /nologo /T ps_5_0 /E ps_mesh /O3 /Fo ${OUT}/basic_ps_mesh.cso ${SH}/basic.hlsl
  COMMAND ${FXC} /nologo /T ps_5_0 /E ps_line /O3 /Fo ${OUT}/basic_ps_line.cso ${SH}/basic.hlsl
  COMMAND ${FXC} /nologo /T ps_5_0 /E ps_depth_alpha /O3 /Fo ${OUT}/basic_ps_depth_alpha.cso ${SH}/basic.hlsl
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets ${OUT}/assets
  COMMENT "Compiling shaders, copying assets")

# Unit-test helper for the community merge logic (header-only src/community_merge.h); compared with tools/hairworks/hwgroom.py
# by tests/community/crosscheck.py.
add_executable(community_merge_test tests/community/community_merge_test.cpp)
target_compile_definitions(community_merge_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(community_merge_test PRIVATE /W3 /utf-8)
