// This file (config.h.in.cmake) is used as a template to generate the
// config.h file in the first step of a cmake build. This file should be
// kept under source control (e.g. svn or git) while config.h can be
// deleted after a suucessful build. After a failed build config.h
// should be examined.

#cmakedefine OS_LINUX  1
#cmakedefine SG_LIB_LINUX 1 
#cmakedefine SG_LIB_ANDROID 1 
#cmakedefine OS_ANDROID  1
#cmakedefine OS_FREEBSD  1
#cmakedefine SG_LIB_FREEBSD  1
#cmakedefine OS_NetBSD  1
#cmakedefine SG_LIB_NETBSD  1
#cmakedefine OS_OPENBSD  1
#cmakedefine SG_LIB_OPENBSD 1 
#cmakedefine OS_SOLARIS 1 
#cmakedefine SG_LIB_SOLARIS 1
#cmakedefine OS_AIX 1
#cmakedefine SG_LIB_AIX 1
#cmakedefine OS_HAIKU 1
#cmakedefine SG_LIB_HAIKU 1
#cmakedefine OS_WIN32 1 
#cmakedefine SG_LIB_WIN32  1
#cmakedefine SG_LIB_MINGW  1
#cmakedefine SG_LIB_CYGWIN  1

#cmakedefine HAVE_POSIX_MEMALIGN 1
#cmakedefine HAVE_POSIX_FADVISE 1
#cmakedefine HAVE_CLOCK_GETTIME 1
#cmakedefine HAVE_GETOPT_H 1
#cmakedefine HAVE_GETOPT_LONG 1
#cmakedefine HAVE_GETRANDOM 1
#cmakedefine HAVE_SYS_RANDOM_H 1
#cmakedefine HAVE_GETTIMEOFDAY 1
#cmakedefine HAVE_SYSCONF 1
#cmakedefine HAVE_LSEEK64 1
#cmakedefine HAVE_SRAND48_R 1
#cmakedefine HAVE_STDATOMIC_H 1
#cmakedefine HAVE_PTHREAD_CANCEL 1
#cmakedefine HAVE_PTHREAD_KILL 1
#cmakedefine HAVE_LINUX_BSG_H 1
#cmakedefine IGNORE_FAST_LEBE 1
#cmakedefine HAVE_BYTESWAP_H 1
#cmakedefine HAVE_LINUX_MAJOR_H 1
#cmakedefine HAVE_LINUX_TYPES_H 1
#cmakedefine HAVE_LINUX_BSG_H 1
#cmakedefine HAVE_LINUX_KDEV_T_H 1
#cmakedefine HAVE_LINUX_NVME_IOCTL_H 1
#cmakedefine HAVE_NVME 1
#cmakedefine HAVE_SYS_TYPES_H 1
#cmakedefine IGNORE_NVME 1
#cmakedefine IGNORE_LINUX_SGV4 1
#cmakedefine SG_SCSI_STRINGS 1
#cmakedefine NEED_GETOPT_H 1
#cmakedefine NEED_GETOPT_LONG 1
#cmakedefine IGNORE_LINUX_BSG 1
#cmakedefine HAVE_SETMODE 1
#cmakedefine WIN32_SPT_DIRECT 1

#define BUILD_TIME "@BUILD_TIME@"

// Some other examples:
// #define SMP_LIB_LINUX @OS_LINUX@
//
// # This will generate a line in the output_file. Then in CMLists.txt:
// #      set(FEATURE_COMMENT "//")
// @FEATURE_COMMENT@#define OPTIONAL_SETTING 1

