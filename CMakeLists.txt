cmake_minimum_required(VERSION 3.20)
project(crashspyro LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CSP_BUILD_TESTS "Build the platform-neutral protocol tests" ON)

# Header-only, platform-neutral pieces shared by everything.
add_library(csp_protocol INTERFACE)
target_include_directories(csp_protocol INTERFACE protocol common)

if(WIN32)
  # Both games are 64-bit, so the bridges must be too.
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Configure a 64-bit build: cmake -B build -A x64")
  endif()

  # Static runtime so players don't need a VC++ redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

  # MinHook v1.3.4 (BSD 2-clause; third_party/minhook/LICENSE.txt).
  add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
  target_include_directories(minhook PUBLIC third_party/minhook/include)

  add_library(csp_common STATIC
    common/config.cpp
    common/crashlog.cpp
    common/hook.cpp
    common/log.cpp
    common/memory.cpp
    common/session.cpp
    common/shm.cpp
    common/standin.cpp
    common/steampad.cpp)
  target_link_libraries(csp_common PUBLIC csp_protocol minhook)
  target_compile_definitions(csp_common PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

  # One bridge per game, each standing in for the XInput DLL that game loads
  # from its own folder, each in its own output folder. The $<1:...> wrapper
  # stops Visual Studio generators appending a per-config subfolder.
  function(csp_bridge target game_dir dll_name)
    add_library(${target} SHARED
      ${game_dir}/src/main.cpp
      common/proxy_xinput.cpp
      ${game_dir}/src/${dll_name}.def)
    target_link_libraries(${target} PRIVATE csp_common)
    target_compile_definitions(${target} PRIVATE CSP_REAL_XINPUT=L"${dll_name}.dll")
    set_target_properties(${target} PROPERTIES
      OUTPUT_NAME ${dll_name}
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_BINARY_DIR}/out/${game_dir}>
      LIBRARY_OUTPUT_DIRECTORY $<1:${CMAKE_BINARY_DIR}/out/${game_dir}>)
  endfunction()

  csp_bridge(csp_crash crash xinput9_1_0)
  csp_bridge(csp_spyro spyro xinput1_3)
  target_sources(csp_spyro PRIVATE spyro/src/ue.cpp)

  add_executable(csp_monitor tools/monitor/main.cpp)
  target_link_libraries(csp_monitor PRIVATE csp_protocol)
  target_compile_definitions(csp_monitor PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(csp_monitor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_BINARY_DIR}/out/tools>)
endif()

if(CSP_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
  add_executable(csp_tests tests/test_protocol.cpp)
  target_link_libraries(csp_tests PRIVATE csp_protocol Threads::Threads)
  target_compile_definitions(csp_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  add_test(NAME protocol COMMAND csp_tests)
endif()
