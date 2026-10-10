cmake_minimum_required(VERSION 3.20)
# The version (semver), optionally with a prerelease suffix like -alpha or -beta.1. Release tags are
# "v" plus this, e.g. v0.1.0-alpha. project() only takes the numeric part.
set(APP_VERSION "0.1.1-alpha")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" APP_VERSION_NUMBER "${APP_VERSION}")
project(SoftwareESSAdapter VERSION ${APP_VERSION_NUMBER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Link the C++ runtime statically so users don't need the VC++ redistributable
# (MSVC) or MinGW's runtime DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MINGW)
  add_link_options(-static)
endif()

# libusb has no CMake build, so compile its Windows sources directly.
# To build offline, point at any libusb checkout, e.g. Dolphin's copy:
#   -DFETCHCONTENT_SOURCE_DIR_LIBUSB=<dolphin>/Externals/libusb/libusb
include(FetchContent)
FetchContent_Declare(libusb
  GIT_REPOSITORY https://github.com/libusb/libusb.git
  GIT_TAG v1.0.27
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libusb)

set(LIBUSB_DIR ${libusb_SOURCE_DIR}/libusb)
add_library(usb STATIC
  ${LIBUSB_DIR}/core.c
  ${LIBUSB_DIR}/descriptor.c
  ${LIBUSB_DIR}/hotplug.c
  ${LIBUSB_DIR}/io.c
  ${LIBUSB_DIR}/strerror.c
  ${LIBUSB_DIR}/sync.c
  ${LIBUSB_DIR}/os/events_windows.c
  ${LIBUSB_DIR}/os/threads_windows.c
  ${LIBUSB_DIR}/os/windows_common.c
  ${LIBUSB_DIR}/os/windows_usbdk.c
  ${LIBUSB_DIR}/os/windows_winusb.c)
target_include_directories(usb PUBLIC ${LIBUSB_DIR})
if(MSVC)
  target_include_directories(usb PRIVATE ${libusb_SOURCE_DIR}/msvc)
else()
  target_include_directories(usb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libusb-mingw)
endif()

add_library(bridge_core STATIC
  src/dsu_server.cpp
  src/ess.cpp
  src/ess_range.cpp
  src/gc_adapter.cpp
  src/notch.cpp
  src/stick.cpp)
target_include_directories(bridge_core PUBLIC src)
target_compile_definitions(bridge_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(bridge_core PUBLIC usb ws2_32)

add_executable(software-ess-adapter WIN32 src/main.cpp assets/resources.rc)
set_target_properties(software-ess-adapter PROPERTIES OUTPUT_NAME "Software ESS Adapter")
target_link_libraries(software-ess-adapter PRIVATE bridge_core dwmapi shell32)
target_compile_definitions(software-ess-adapter PRIVATE APP_VERSION="${APP_VERSION}")

add_executable(bridge-tests test/bridge_tests.cpp)
target_link_libraries(bridge-tests PRIVATE bridge_core)
