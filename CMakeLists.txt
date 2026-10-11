cmake_minimum_required(VERSION 3.20)
# The version (semver), optionally with a prerelease suffix like -alpha or -beta.1. Release tags are
# "v" plus this, e.g. v0.1.0-alpha. project() only takes the numeric part.
set(APP_VERSION "0.3.0-alpha")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" APP_VERSION_NUMBER "${APP_VERSION}")
project(SoftwareESSAdapter VERSION ${APP_VERSION_NUMBER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Link the C++ runtime statically so users don't need the VC++ redistributable
# (MSVC) or MinGW's runtime DLLs. On Linux only libstdc++ goes in statically: glibc and the
# desktop libraries come from the system.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MINGW)
  add_link_options(-static)
elseif(UNIX)
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

include(FetchContent)
if(WIN32)
  # libusb has no CMake build, so compile its Windows sources directly.
  # To build offline, point at any libusb checkout, e.g. Dolphin's copy:
  #   -DFETCHCONTENT_SOURCE_DIR_LIBUSB=<dolphin>/Externals/libusb/libusb
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
else()
  # Every Linux desktop has libusb, so use the system's (libusb-1.0-0-dev to build).
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
  add_library(usb INTERFACE)
  target_link_libraries(usb INTERFACE PkgConfig::LIBUSB)
endif()

# FLTK draws the windows: only the core library, linked statically. On Windows it draws with
# plain GDI (no GDI+ antialiasing) so the plots look as they did before FLTK. On Linux it runs on
# Wayland, or X11 where there is no Wayland.
set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLTK_OPTIONS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FORMS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_GL OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_HTML_DOCS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_PDF_DOCS OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_SVG OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_PRINT_SUPPORT OFF CACHE BOOL "" FORCE)
set(FLTK_MSVC_RUNTIME_DLL OFF CACHE BOOL "" FORCE)
set(FLTK_GRAPHICS_GDIPLUS OFF CACHE BOOL "" FORCE)
# Image readers (the Linux window icon is a PNG) and libdecor (Wayland title bars) built in rather
# than taken from the system.
set(FLTK_USE_SYSTEM_LIBPNG OFF CACHE BOOL "" FORCE)
set(FLTK_USE_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
set(FLTK_USE_SYSTEM_LIBJPEG OFF CACHE BOOL "" FORCE)
set(FLTK_USE_SYSTEM_LIBDECOR OFF CACHE BOOL "" FORCE)
# cmake/patch-fltk.cmake fixes FLTK's Wayland build in a folder whose path has a space.
FetchContent_Declare(fltk
  GIT_REPOSITORY https://github.com/fltk/fltk.git
  GIT_TAG release-1.4.5
  GIT_SHALLOW TRUE
  PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-fltk.cmake)
FetchContent_MakeAvailable(fltk)

# Fonts, license texts, and the Linux window icon, built into the program (src/embedded.hpp).
set(EMBEDDED_FILES
  GPL_TEXT=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md
  THIRD_PARTY_TEXT=${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/third-party.txt
  SELAWIK_TTF=${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Selawik-Regular.ttf
  SELAWIK_SEMIBOLD_TTF=${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Selawik-Semibold.ttf
  CASCADIA_MONO_TTF=${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/CascadiaMono-SemiLight.ttf
  ICON_PNG=${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png)
set(EMBEDDED_SOURCES)
foreach(entry IN LISTS EMBEDDED_FILES)
  string(REGEX REPLACE "^[A-Z_]+=" "" path "${entry}")
  list(APPEND EMBEDDED_SOURCES "${path}")
endforeach()
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded.cpp
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/embedded.cpp "-DFILES=${EMBEDDED_FILES}"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${EMBEDDED_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  VERBATIM)

add_library(bridge_core STATIC
  src/delayed_sender.cpp
  src/dsu_server.cpp
  src/ess.cpp
  src/ess_range.cpp
  src/gc_adapter.cpp
  src/ini.cpp
  src/notch.cpp
  src/soh.cpp
  src/stick.cpp)
target_include_directories(bridge_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(bridge_core PUBLIC usb Threads::Threads)
if(WIN32)
  target_compile_definitions(bridge_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(bridge_core PUBLIC ws2_32)
endif()

add_executable(software-ess-adapter WIN32 src/main.cpp ${CMAKE_CURRENT_BINARY_DIR}/embedded.cpp)
target_link_libraries(software-ess-adapter PRIVATE bridge_core fltk::fltk)
if(WIN32)
  set_target_properties(software-ess-adapter PROPERTIES OUTPUT_NAME "Software ESS Adapter")
  target_sources(software-ess-adapter PRIVATE assets/resources.rc)
  target_link_libraries(software-ess-adapter PRIVATE dwmapi shell32)
else()
  # The usual Linux name, without spaces. The fonts go to fontconfig, and FLTK's PNG reader loads
  # the window icon.
  set_target_properties(software-ess-adapter PROPERTIES OUTPUT_NAME "software-ess-adapter")
  find_package(Fontconfig REQUIRED)
  target_link_libraries(software-ess-adapter PRIVATE fltk::images Fontconfig::Fontconfig)
endif()
target_compile_definitions(software-ess-adapter PRIVATE APP_VERSION="${APP_VERSION}")
# Ship of Harkinian output and input lag, off until SoH's DSU input is in an official libultraship
# release. build.ps1 --dev turns it on.
option(SOH_DEV "Build the Ship of Harkinian features" OFF)
if(SOH_DEV)
  target_compile_definitions(software-ess-adapter PRIVATE SOH_DEV)
endif()

add_executable(bridge-tests test/bridge_tests.cpp)
target_link_libraries(bridge-tests PRIVATE bridge_core)
