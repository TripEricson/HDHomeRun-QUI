cmake_minimum_required(VERSION 3.16)   # Qt 6 needs 3.16+

# The version shown in the window title -- change it here only
project(HDHomeRun-QUI VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 6 is required (the code uses Qt 6 APIs)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

# --- LIBHDHOMERUN ---
# libhdhomerun is SiliconDust's library and isn't part of this repository.
# Either:
#   1. clone it into this folder (built from source along with the program):
#        git clone https://github.com/Silicondust/libhdhomerun.git
#   2. or install your distribution's package, e.g. on Ubuntu/Debian:
#        sudo apt install libhdhomerun-dev
# A local libhdhomerun/ folder takes priority if both are present.
set(HDHOMERUN_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libhdhomerun")
set(HDHOMERUN_SRCS "")

if(EXISTS "${HDHOMERUN_LOCAL_DIR}/hdhomerun_device.c")
    message(STATUS "libhdhomerun: building from ${HDHOMERUN_LOCAL_DIR}")
    # Listed explicitly so the Windows-only files aren't compiled on Linux
    set(HDHOMERUN_SRCS
        libhdhomerun/hdhomerun_channels.c
        libhdhomerun/hdhomerun_channelscan.c
        libhdhomerun/hdhomerun_control.c
        libhdhomerun/hdhomerun_debug.c
        libhdhomerun/hdhomerun_device.c
        libhdhomerun/hdhomerun_device_selector.c
        libhdhomerun/hdhomerun_discover.c
        libhdhomerun/hdhomerun_os_posix.c
        libhdhomerun/hdhomerun_pkt.c
        libhdhomerun/hdhomerun_sock.c
        libhdhomerun/hdhomerun_sock_posix.c
        libhdhomerun/hdhomerun_video.c
    )
    # OS-specific network detection (matches logic in the TUI Makefile)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND HDHOMERUN_SRCS libhdhomerun/hdhomerun_sock_netlink.c)
    else()
        # macOS / BSD / others use getifaddrs
        list(APPEND HDHOMERUN_SRCS libhdhomerun/hdhomerun_sock_getifaddrs.c)
    endif()
    set(HDHOMERUN_INCLUDE_DIR "${HDHOMERUN_LOCAL_DIR}")
    set(HDHOMERUN_LIBRARY "")
else()
    find_path(HDHOMERUN_INCLUDE_DIR hdhomerun.h PATH_SUFFIXES libhdhomerun)
    find_library(HDHOMERUN_LIBRARY hdhomerun)
    if(NOT HDHOMERUN_INCLUDE_DIR OR NOT HDHOMERUN_LIBRARY)
        message(FATAL_ERROR
            "libhdhomerun not found. Either clone it into this folder:\n"
            "    git clone https://github.com/Silicondust/libhdhomerun.git\n"
            "or install your distribution's package (Ubuntu/Debian: sudo apt install libhdhomerun-dev),\n"
            "then run cmake again.")
    endif()
    message(STATUS "libhdhomerun: using system library ${HDHOMERUN_LIBRARY}")
endif()

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        # Shared L1 parser files from the TUI / RENDER (use unchanged)
        l1_detail_parser.c
        l1_detail_parser.h
        atsc3_tables.h
        l1_details.c
        l1_details.h
        # QUI's libhdhomerun implementation of the tuner calls l1_details.c needs
        tuner_qui.c
        tuner.h
        ${HDHOMERUN_SRCS}
)

qt_add_executable(HDHomeRun-QUI
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
)

target_compile_definitions(HDHomeRun-QUI PRIVATE QUI_VERSION="${PROJECT_VERSION}")

target_link_libraries(HDHomeRun-QUI PRIVATE
    Qt6::Widgets
    Qt6::Network
)

target_include_directories(HDHomeRun-QUI PRIVATE
    ${HDHOMERUN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# System libhdhomerun (when not building it from a local libhdhomerun/ folder)
if(HDHOMERUN_LIBRARY)
    target_link_libraries(HDHomeRun-QUI PRIVATE ${HDHOMERUN_LIBRARY})
endif()

# Linux requires linking against Realtime library (rt) for clock_gettime
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(HDHomeRun-QUI PRIVATE rt)
endif()

# The L1 parser uses pow/log10/isnan (LDM SNR math), which need libm on Unix
if(UNIX AND NOT APPLE)
    target_link_libraries(HDHomeRun-QUI PRIVATE m)
endif()

set_target_properties(HDHomeRun-QUI PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS HDHomeRun-QUI
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_finalize_executable(HDHomeRun-QUI)
