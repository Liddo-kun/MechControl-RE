cmake_minimum_required(VERSION 3.25)
project(MechControlNative VERSION 0.1.0 LANGUAGES CXX RC)
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Use the MSVC x64 toolchain.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
# Keep release diagnostics outside the runtime package without sacrificing optimization.
add_compile_options($<$<CONFIG:Release>:/Zi>)
add_link_options($<$<CONFIG:Release>:/DEBUG:FULL> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
option(MECHCONTROL_WITH_CLI "Build the optional native CLI server, mcctl client and protocol fixture" OFF)
include(CTest)
include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)
add_library(configuration STATIC src/config_json.cpp src/config_text.cpp src/profiles.cpp src/settings.cpp src/identity.cpp)
target_include_directories(configuration PUBLIC src)
# The reference targets net8.0-windows10.0.19041.0; system ICU is available there.
target_compile_definitions(configuration PRIVATE NTDDI_VERSION=NTDDI_WIN10_VB)
target_link_libraries(configuration PUBLIC nlohmann_json::nlohmann_json PRIVATE wbemuuid ole32 oleaut32 icu)
add_library(ec STATIC src/ec.cpp src/ec_session.cpp src/thermal.cpp src/controller.cpp src/controller_cli.cpp
  src/nvidia_bios.cpp src/nvidia_api.cpp src/nvidia_store.cpp src/nvidia_power.cpp src/controller_gpu.cpp
  src/peripherals.cpp src/cooler.cpp)
target_include_directories(ec PUBLIC src)
target_link_libraries(ec PUBLIC configuration PRIVATE setupapi cfgmgr32 shell32 advapi32 hid windowsapp user32)

get_filename_component(SDK_BIN "${CMAKE_RC_COMPILER}" DIRECTORY)
get_filename_component(WINDOWS_KITS "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
find_program(CTRPP ctrpp HINTS "${SDK_BIN}"
  "${WINDOWS_KITS}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64" REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mechcontrol_counters.h" "${CMAKE_CURRENT_BINARY_DIR}/counters.rc"
  COMMAND "${CTRPP}" -o "${CMAKE_CURRENT_BINARY_DIR}/mechcontrol_counters.h"
    -rc "${CMAKE_CURRENT_BINARY_DIR}/counters.rc" "${CMAKE_CURRENT_SOURCE_DIR}/resources/counters.man"
  DEPENDS resources/counters.man VERBATIM)
add_library(foundation STATIC src/log.cpp src/worker.cpp src/counters.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/mechcontrol_counters.h")
target_include_directories(foundation PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(foundation PUBLIC advapi32 windowsapp ec)
add_executable(MechControl WIN32 src/ui.cpp src/fan_chart.cpp resources/app.rc
  "${CMAKE_CURRENT_BINARY_DIR}/counters.rc")
target_link_libraries(MechControl PRIVATE foundation comctl32 shell32 gdi32 comdlg32)
target_link_options(MechControl PRIVATE /MANIFEST:EMBED
  "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
  /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest)
add_custom_command(TARGET MechControl POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Profiles/MECHREVO"
    "$<TARGET_FILE_DIR:MechControl>/Profiles/MECHREVO")

# Registration is explicit and temporary. Nothing registers counters at application startup.
file(READ resources/counters.man COUNTER_MANIFEST)
# Lodctr resolves applicationIdentity relative to this manifest's directory.
# Forward-slash absolute paths are not recognized as absolute by lodctr.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/MechControl-counters.man" CONTENT "${COUNTER_MANIFEST}")
if(BUILD_TESTING)
  add_executable(peripheral-check tests/peripheral_check.cpp)
  target_link_libraries(peripheral-check PRIVATE foundation)
  add_dependencies(peripheral-check MechControl)
  add_test(NAME peripheral COMMAND peripheral-check)
  add_executable(nvidia-check tests/nvidia_check.cpp)
  target_link_libraries(nvidia-check PRIVATE ec)
  add_test(NAME nvidia COMMAND nvidia-check)
  add_executable(controller-check tests/controller_check.cpp)
  target_link_libraries(controller-check PRIVATE foundation)
  add_dependencies(controller-check MechControl)
  add_test(NAME controller COMMAND controller-check)
  add_executable(ec-check tests/ec_check.cpp)
  target_link_libraries(ec-check PRIVATE foundation)
  add_dependencies(ec-check MechControl)
  add_test(NAME ec COMMAND ec-check)
  add_executable(ui-pages-check WIN32 src/ui.cpp src/fan_chart.cpp resources/app.rc "${CMAKE_CURRENT_BINARY_DIR}/counters.rc")
  target_compile_definitions(ui-pages-check PRIVATE MECHCONTROL_PAGE_CHECK)
  target_link_libraries(ui-pages-check PRIVATE foundation comctl32 shell32 gdi32 comdlg32 dwmapi)
  target_link_options(ui-pages-check PRIVATE /MANIFEST:EMBED "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'" /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest)
  add_dependencies(ui-pages-check MechControl)
  add_executable(configuration-check tests/configuration_check.cpp)
  target_link_libraries(configuration-check PRIVATE configuration)
  add_test(NAME configuration COMMAND configuration-check "${CMAKE_CURRENT_SOURCE_DIR}/Profiles/MECHREVO"
    "C:/Temp/tandem/mechcontrol-native-phase2/check-$<CONFIG>")
  add_executable(foundation-check tests/foundation_check.cpp "${CMAKE_CURRENT_BINARY_DIR}/counters.rc")
  target_link_libraries(foundation-check PRIVATE foundation pdh)
  add_test(NAME foundation COMMAND foundation-check)
endif()
if(MECHCONTROL_WITH_CLI)
  add_library(cli_protocol STATIC src/protocol.cpp)
  target_include_directories(cli_protocol PUBLIC src)
  target_link_libraries(cli_protocol PUBLIC advapi32)
  target_sources(MechControl PRIVATE src/cli_server.cpp)
  target_compile_definitions(MechControl PRIVATE MECHCONTROL_WITH_CLI)
  target_link_libraries(MechControl PRIVATE cli_protocol)
  add_executable(mcctl src/mcctl.cpp)
  target_link_libraries(mcctl PRIVATE cli_protocol)
  if(BUILD_TESTING)
    target_sources(controller-check PRIVATE src/cli_server.cpp)
    target_compile_definitions(controller-check PRIVATE MECHCONTROL_WITH_CLI)
    target_link_libraries(controller-check PRIVATE cli_protocol)
    add_executable(protocol-check tests/protocol_check.cpp)
    target_link_libraries(protocol-check PRIVATE cli_protocol)
    add_test(NAME protocol COMMAND protocol-check)
  endif()
endif()
install(TARGETS MechControl RUNTIME DESTINATION . COMPONENT Runtime)
install(FILES "$<TARGET_PDB_FILE:MechControl>" DESTINATION . COMPONENT DeveloperSymbols CONFIGURATIONS Release)
if(MECHCONTROL_WITH_CLI)
  install(TARGETS mcctl RUNTIME DESTINATION . COMPONENT Runtime)
  install(FILES "$<TARGET_PDB_FILE:mcctl>" DESTINATION . COMPONENT DeveloperSymbols CONFIGURATIONS Release)
endif()
install(DIRECTORY Profiles/MECHREVO DESTINATION Profiles COMPONENT Runtime FILES_MATCHING PATTERN "*.json")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/MechControl-counters.man"
  THIRD_PARTY_NOTICES.md tools/counters.ps1 tools/handoff.ps1 DESTINATION . COMPONENT Runtime)
install(FILES docs/architecture.md docs/build.md docs/handoff.md DESTINATION docs COMPONENT Runtime)
install(FILES docs/release.md DESTINATION . RENAME README.md COMPONENT Runtime)
