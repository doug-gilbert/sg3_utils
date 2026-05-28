cmake_minimum_required ( VERSION 3.10 )

project(sg3_utils VERSION 1.48.9 LANGUAGES C)
# Use version number: x.y.9 to indicate pre-release of x.(y+1)
# Example: VERSION 1.48.9 is pre-release of 1.49

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set ( CMAKE_C_STANDARD 99 )
set ( CMAKE_C_STANDARD_REQUIRED ON )
set ( CMAKE_C_EXTENSIONS OFF )
set ( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -g -Wall -W" )

add_compile_definitions ( _LARGEFILE64_SOURCE _FILE_OFFSET_BITS=64 )

string(TIMESTAMP BUILD_TIME "%Y%m%d %H:%M:%S")

option ( BUILD_SHARED_LIBS "Build using shared libraries" ON)
option ( ENABLE_DEBUG "Turn on debugging (extra warnings)" OFF)
option ( DISABLE_FAST_LEBE "Use generic little-endian/big-endian code instead" OFF)
option ( ENABLE_PT_DUMMY "pass-through code compiles, does nothing" OFF)
option ( ENABLE_WIN32_SPT_DIRECT "enable Win32 SPT Direct" OFF)
option ( DISABLE_SCSISTRINGS "Disable full SCSI sense strings and NVMe status strings" OFF)
option ( DISABLE_NVME_SUPP "remove all or most NVMe code" OFF)
option ( DISABLE_LINUX_SGV4 "for Linux sg driver avoid v4 interface even if available" OFF)
option ( DISABLE_LINUXBSG "option ignored, this is placeholder" OFF)

# The intention is to support both cmake _and_ GNU Autotools build systems.
# To this end (and to shorten the actual compile line) a dynamic config.h
# is constructed to pass user options and detected system characteristics
# (e.g. is <byteswap.h> present on the standard header search path? ).
# In Autotools the initial call to autoconf (often in a script called
# autogen.sh or bootstrap) creates an artifact file call config.h.in
# (and a script called configure). The next step is to execute that configure
# script (typically with './configure'), perhaps with some options (to list
# those options use './configure --help'). Part of the work of configure is
# to use config.h.in to create config.h . The next step is to compile the
# code and that code looks for that config.h file. With cmake the goal is to
# also generate a config.h file which is functional equivalent to what
# Autotools generates, given the same environment and command line options
# (if any). To achieve this for cmake, a _permanent_ config.h.in.cmake file
# has been added to the project for this cmake line (in CMakeLists.txt):
#    configure_file( config.h.in.cmake config.h )
# and that generates a suitable config.h . Clear?
#
# Currently the only parameters (a define in C) that go through the compile
# line are DEBUG (more precisely '-DDEBUG' will appear on the compile line
# if ENABLE_DEBUG=ON is given (e.g. 'cmake . -DENABLE_DEBUG=ON') and
# HAVE_CONFIG_H . The rest of the parameters/defines are sent through
# config.h .
#
# So config.h.in.cmake is a "magic" file associated with this cmake
# implementation. The other cmake related files are a small hierarchy of
# CMakeList.txt files. The main CMakeList.txt is in the top-level directory
# of this package. The other magic file is lib/cmake/sgutils2Config.cmake.in
# whose purpose remains a mystery.
#
# For both autotools and cmake in-tree builds, config.h is placed in the top
# level source directory. For out-of-tree builds the config.h is placed in
# the top level build directory (e.g. <top_level_src>/build/config.h ).

set(FEATURE_COMMENT "//")

if ( ENABLE_DEBUG )
    MESSAGE( STATUS ">> Compile with DEBUG set" )
    # if enabled, DEBUG define passed on each compile line
    # Debug actions in lib/CMakeLists.txt and src/CMakeLists.txt
endif ( ENABLE_DEBUG )

set(IGNORE_FAST_LEBE 0)
if (DISABLE_FAST_LEBE)
    set(IGNORE_FAST_LEBE 1)
endif (DISABLE_FAST_LEBE)

set(IGNORE_NVME 0)
if (DISABLE_NVME_SUPP)
    set(IGNORE_NVME 1)
endif (DISABLE_NVME_SUPP)

set(IGNORE_LINUX_SGV4 0)
if (DISABLE_LINUX_SGV4)
    set(IGNORE_LINUX_SGV4 1)
endif (DISABLE_LINUX_SGV4)

set(SG_SCSI_STRINGS 1)
if (DISABLE_SCSISTRINGS)
    set(SG_SCSI_STRINGS 0)
endif (DISABLE_SCSISTRINGS)

set(PT_DUMMY 0)
if (ENABLE_PT_DUMMY)
    set(PT_DUMMY 1)
endif (ENABLE_PT_DUMMY)

set(IGNORE_LINUX_BSG 0)
if (DISABLE_LINUXBSG)
    set(IGNORE_LINUX_BSG 1)
endif (DISABLE_LINUXBSG)

set(WIN32_SPT_DIRECT 0)
if (ENABLE_WIN32_SPT_DIRECT)
    set(WIN32_SPT_DIRECT 1)
endif (ENABLE_WIN32_SPT_DIRECT)

include ( CheckIncludeFile )
## include(CheckCXXSourceCompiles)
CHECK_INCLUDE_FILE( "byteswap.h" HAVE_BYTESWAP_H )
CHECK_INCLUDE_FILE( "getopt.h" HAVE_GETOPT_H )    # non-standard but common
CHECK_INCLUDE_FILE( "sys/random.h" HAVE_SYS_RANDOM_H )
CHECK_INCLUDE_FILE( "stdatomic.h" HAVE_STDATOMIC_H )
CHECK_INCLUDE_FILE( "linux/bsg.h" HAVE_LINUX_BSG_H )
CHECK_INCLUDE_FILE( "linux/nvme_ioctl.h" HAVE_LINUX_NVME_IOCTL_H )
CHECK_INCLUDE_FILE( "sys/types.h" HAVE_SYS_TYPES_H )
CHECK_INCLUDE_FILE( "getopt.h" HAVE_GETOPT_H )

include(CheckFunctionExists)

check_function_exists(posix_memalign HAVE_POSIX_MEMALIGN)
check_function_exists(posix_fadvise HAVE_POSIX_FADVISE)
check_function_exists(clock_gettime HAVE_CLOCK_GETTIME)
check_function_exists(getopt_long HAVE_GETOPT_LONG)
check_function_exists(getrandom HAVE_GETRANDOM)
check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
check_function_exists(sysconf HAVE_SYSCONF)
check_function_exists(lseek64 HAVE_LSEEK64)
check_function_exists(srand48_r HAVE_SRAND48_R)
check_function_exists(pthread_cancel HAVE_PTHREAD_CANCEL)
check_function_exists(pthread_kill HAVE_PTHREAD_KILL)
check_function_exists(getopt_long HAVE_GETOPT_LONG)
check_function_exists(setmode HAVE_SETMODE)

find_package(Threads)

include(CheckLibraryExists)
check_library_exists(rt clock_gettime "" NEED_RT_LIB)

set(OS_LINUX 0)
set(SG_LIB_LINUX 0)
set(SG_LIB_ANDROID 0)
set(OS_ANDROID 0)
set(OS_FREEBSD 0)
set(SG_LIB_FREEBSD 0)
set(OS_NetBSD 0)
set(SG_LIB_NETBSD 0)
set(OS_OPENBSD 0)
set(SG_LIB_OPENBSD 0)
set(OS_SOLARIS 0)
set(SG_LIB_SOLARIS 0)
set(OS_AIX 0)
set(OS_WIN32 0)  # OS_WIN32_MINGW + OS_WIN32_CYGWIN only used in Makefile.am
set(SG_LIB_WIN32 0)
set(SG_LIB_MINGW 0)
set(SG_LIB_CYGWIN 0)   # not used currently, may use in future

set(NEED_GETOPT_H 1)
if ( HAVE_GETOPT_H )
    set(NEED_GETOPT_H 0)
endif ( HAVE_GETOPT_H )

set(NEED_GETOPT_LONG 1)
if ( HAVE_GETOPT_LONG )
    set(NEED_GETOPT_LONG 0)
endif ( HAVE_GETOPT_LONG )


message(STATUS ">> cmake System Name: ${CMAKE_SYSTEM_NAME}")
if (CYGWIN)
    message(STATUS ">> Cygwin build")
    set(OS_WIN32 1)
    set(SG_LIB_WIN32 1)
    set(SG_LIB_CYGWIN 1)
    set(HAVE_NVME 1)
elseif (MINGW)
    message(STATUS ">> MinGW build")
    set(OS_WIN32 1)
    set(SG_LIB_WIN32 1)
    set(SG_LIB_MINGW 1)
    set(HAVE_NVME 1)
elseif (WIN32)
    message(STATUS ">> Windows build")
    set(OS_WIN32 1)
    set(SG_LIB_WIN32 1)
elseif (ANDROID)
    message(STATUS ">> Android build")
    set(OS_ANDROID 1)
    set(SG_LIB_LINUX 1)
    set(SG_LIB_ANDROID 1)
    if (HAVE_LINUX_NVME_IOCTL_H)
        set(HAVE_NVME 1)
    endif ()
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS ">> Linux build")
    set(OS_LINUX 1)
    set(SG_LIB_LINUX 1)
    CHECK_INCLUDE_FILE( "linux/major.h" HAVE_LINUX_MAJOR_H )
    CHECK_INCLUDE_FILE( "linux/types.h" HAVE_LINUX_TYPES_H )
    CHECK_INCLUDE_FILE( "linux/bsg.h" HAVE_LINUX_BSG_H )
    CHECK_INCLUDE_FILE( "linux/kdev_t.h" HAVE_LINUX_KDEV_T_H )
    if (HAVE_LINUX_NVME_IOCTL_H)
        set(HAVE_NVME 1)
    endif ()
elseif (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    message(STATUS ">> FreeBSD build")
    set(OS_FREEBSD 1)
    set(SG_LIB_FREEBSD 1)
    set(HAVE_NVME 1)
elseif (CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    message(STATUS ">> NetBSD build")
    set(OS_NETBSD 1)
    set(SG_LIB_NETBSD 1)
elseif (CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    message(STATUS ">> OpenBSD build")
    set(OS_OPENBSD 1)
    set(SG_LIB_OPENBSD 1)
elseif (CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    message(STATUS ">> Solaris build")
    set(OS_SOLARIS 1)
    set(SG_LIB_SOLARIS 1)
elseif (CMAKE_SYSTEM_NAME STREQUAL "AIX")
    message(STATUS ">> AIX build")
    set(OS_AIX 1)
    set(SG_LIB_AIX 1)
elseif (CMAKE_SYSTEM_NAME STREQUAL "HAIKU")
    message(STATUS ">> HAIKU build")
    set(OS_HAIKU 1)
    set(SG_LIB_HAIKU 1)
elseif (APPLE)
    message(STATUS ">> Apply (macOS) build")
    set(OS_MACOS 1)
else()
    message(STATUS "Unknown system: ${CMAKE_SYSTEM_NAME}")
endif()

configure_file( ${CMAKE_SOURCE_DIR}/config.h.in.cmake ${CMAKE_BINARY_DIR}/config.h )
add_definitions ( -DHAVE_CONFIG_H )


add_subdirectory( include )

add_subdirectory( lib )

add_subdirectory( src )

add_subdirectory( scripts )

add_subdirectory( doc )

install(FILES ChangeLog DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT utilities)
install(FILES README DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT utilities)
install(FILES README.details DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT utilities)
install(FILES COVERAGE DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT utilities)
install(FILES CREDITS DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT utilities)
install(FILES AUTHORS DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT utilities)
install(FILES sg3_utils.man8.html DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT utilities)

#
#  CPACK (build packages for some distros)
#
set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VENDOR "Douglas Gilbert")
set(CPACK_PACKAGE_CONTACT "dgilbert@interlog.com")
set(CPACK_PROJECT_VERSION "${PROJECT_VERSION}")

set(CPACK_COMPONENTS_ALL
    runtime
    development
    utilities
)

set(CPACK_COMPONENT_RUNTIME_DESCRIPTION
    "libsgutils2 runtime library for sg3_utils")
set(CPACK_COMPONENT_DEVELOPMENT_DESCRIPTION
    "libsgutils2 development library and include files for sg3_utils")
set(CPACK_COMPONENTS_GROUPING IGNORE)

#
# DEB
#

set(CPACK_DEBIAN_UTILITIES_PACKAGE_NAME "sg3-utils")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "libsgutils2-2")
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_NAME "libsgutils2-dev")
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_DEPENDS
    "libsgutils2-2 (= ${PROJECT_VERSION})")
SET(CPACK_DEBIAN_PACKAGE_DESCRIPTION "SCSI utilities, typically one utility per SCSI command, outputs responses in human readable or JSON form")
SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "Douglas Gilbert")
set(CPACK_DEBIAN_UTILITIES_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_SECTION "libdevel")
SET(CPACK_DEBIAN_PACKAGE_DEPENDS " ")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

SET(CPACK_DEB_COMPONENT_INSTALL YES)

#
# RPM
#

set(CPACK_RPM_UTILITIES_PACKAGE_NAME "sg3_utils")
set(CPACK_RPM_RUNTIME_PACKAGE_NAME "sg3_utils-libs")
set(CPACK_RPM_DEVELOPMENT_PACKAGE_NAME "sg3_utils-devel")
SET(CPACK_RPM_PACKAGE_DESCRIPTION "SCSI utilities, typically one utility per SCSI command, outputs responses in human readable or JSON form")
SET(CPACK_RPM_PACKAGE_DEPENDS " ")
SET(CPACK_RPM_PACKAGE_LICENSE "BSD-2-Clause and GPL-2.0-or-later")
SET(CPACK_RPM_PACKAGE_VENDOR "Douglas Gilbert")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)

set(CPACK_RPM_COMPONENT_INSTALL YES)

#
# FreeBSD
#

set(CPACK_FREEBSD_PACKAGE_LICENSE "BSD-2-clause")
set(CPACK_FREEBSD_PACKAGE_ORIGIN "github.com/doug-gilbert/sg3_utils")
set(CPACK_FREEBSD_PACKAGE_MAINTAINER "dgilbert@interlog.com")
set(CPACK_FREEBSD_PACKAGE_WWW "https://doug-gilbert.github.io/sg3_utils.html")

if ( SG_LIB_LINUX )
    set(CPACK_GENERATOR "")
    find_program(DPKG_EXECUTABLE dpkg)
    if (DPKG_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "DEB")
    endif ()
    find_program(RPMBUILD_EXECUTABLE rpmbuild)
    if (RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
    endif ()
elseif ( SG_LIB_FREEBSD )
    SET(CPACK_GENERATOR "FREEBSD")
endif ( SG_LIB_LINUX )

include(CPack)
include(CTest)
