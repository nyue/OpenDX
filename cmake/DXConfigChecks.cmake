# Platform checks behind dxconfig.h. Covers what the executive reads; the
# names match the autotools build so the sources need no changes.

include(CheckIncludeFile)
include(CheckSymbolExists)

foreach(header
    arpa/inet.h crypt.h ctype.h dlfcn.h errno.h fcntl.h limits.h netdb.h
    net/if.h netinet/in.h pwd.h signal.h stdlib.h string.h strings.h
    sys/errno.h sys/file.h sys/ioctl.h sys/param.h sys/select.h sys/shm.h
    sys/signal.h sys/socket.h sys/stat.h sys/timeb.h sys/time.h sys/times.h
    sys/types.h sys/un.h sys/utsname.h sys/wait.h time.h unistd.h values.h
    wait.h)
  string(TOUPPER "HAVE_${header}" var)
  string(REGEX REPLACE "[/.]" "_" var "${var}")
  check_include_file("${header}" ${var})
endforeach()

set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(finite       "math.h"        HAVE_FINITE)
check_symbol_exists(isnan        "math.h"        HAVE_ISNAN)
check_symbol_exists(M_PI         "math.h"        HAS_M_PI)
check_symbol_exists(M_SQRT2      "math.h"        HAS_M_SQRT2)
check_symbol_exists(fork         "unistd.h"      HAVE_FORK)
check_symbol_exists(getdtablesize "unistd.h"     HAVE_GETDTABLESIZE)
check_symbol_exists(getopt       "unistd.h"      HAVE_GETOPT)
check_symbol_exists(isatty       "unistd.h"      HAVE_ISATTY)
check_symbol_exists(pipe         "unistd.h"      HAVE_PIPE)
check_symbol_exists(sysconf      "unistd.h"      HAVE_SYSCONF)
check_symbol_exists(unlink       "unistd.h"      HAVE_UNLINK)
check_symbol_exists(popen        "stdio.h"       HAVE_POPEN)
check_symbol_exists(pclose       "stdio.h"       HAVE_PCLOSE)
check_symbol_exists(rand         "stdlib.h"      HAVE_RAND)
check_symbol_exists(srand        "stdlib.h"      HAVE_SRAND)
check_symbol_exists(random       "stdlib.h"      HAVE_RANDOM)
check_symbol_exists(srandom      "stdlib.h"      HAVE_SRANDOM)
check_symbol_exists(RAND_MAX     "stdlib.h"      HAVE_RAND_MAX)
check_symbol_exists(strcasecmp   "strings.h"     HAVE_STRCASECMP)
check_symbol_exists(setsockopt   "sys/socket.h"  HAVE_SETSOCKOPT)
check_symbol_exists(shmat        "sys/shm.h"     HAVE_SHMAT)
check_symbol_exists(SIGPIPE      "signal.h"      HAVE_SIGPIPE)
check_symbol_exists(SIGQUIT      "signal.h"      HAVE_SIGQUIT)
check_symbol_exists(S_ISDIR      "sys/stat.h"    HAS_S_ISDIR)
check_symbol_exists(getlogin     "unistd.h"      HAVE_GETLOGIN)
check_symbol_exists(getpid       "unistd.h"      HAVE_GETPID)
check_include_file(malloc.h HAVE_MALLOC_H)
unset(CMAKE_REQUIRED_DEFINITIONS)

# Shell DXLink uses to start processes.
find_program(DX_BSH NAMES sh PATHS /usr/bin /bin NO_DEFAULT_PATH)

# Same encoding as configure.ac: "04.4.4000" for 4.4.4.
set(DXD_VERSION_STRING
    "0${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}000")

configure_file(${CMAKE_CURRENT_LIST_DIR}/dxconfig.h.cmake
               ${CMAKE_BINARY_DIR}/include/dxconfig.h)
