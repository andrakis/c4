#ifndef C4CC
#ifndef __C4M_UTIL_C
#define __C4M_UTIL_C 1
// This line **must** come **before** including <time.h> in order to
// bring in the POSIX functions such as `clock_gettime() from <time.h>`!
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L


#include <stdint.h> // `UINT64_MAX`
#include <time.h>   // `clock_gettime()` and `timespec_get()`

#include "c4.h"

/// Convert seconds to milliseconds
#define SEC_TO_MS(sec) ((sec)*1000)
/// Convert seconds to microseconds
#define SEC_TO_US(sec) ((sec)*1000000)
/// Convert seconds to nanoseconds
#define SEC_TO_NS(sec) ((sec)*1000000000)

/// Convert nanoseconds to seconds
#define NS_TO_SEC(ns)   ((ns)/1000000000)
/// Convert nanoseconds to milliseconds
#define NS_TO_MS(ns)    ((ns)/1000000)
/// Convert nanoseconds to microseconds
#define NS_TO_US(ns)    ((ns)/1000)

// Other options:
//  CLOCK_MONOTONIC_RAW
//#define CLOCK_MODE      CLOCK_REALTIME
#define CLOCK_MODE      CLOCK_MONOTONIC_RAW

// MSYS2 and Cygwin have CLOCK_MONOTONIC, and no _RAW
#if !defined(CLOCK_MONOTONIC_RAW) && defined(CLOCK_MONOTONIC)
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

// On MSVC, clock_gettime doesn't exist, nor do the various monotonic / realtime options
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC_RAW 0
int clock_gettime(int unused, struct timespec* tv)
{
	return timespec_get(tv, TIME_UTC);
}
#endif

/// Get a time stamp in milliseconds.
static uint64_t millis()
{
    struct timespec ts;
    clock_gettime(CLOCK_MODE, &ts);
    uint64_t ms = SEC_TO_MS((uint64_t)ts.tv_sec) + NS_TO_MS((uint64_t)ts.tv_nsec);
    return ms;
}

/// Get a time stamp in microseconds.
static uint64_t micros()
{
    struct timespec ts;
    clock_gettime(CLOCK_MODE, &ts);
    uint64_t us = SEC_TO_US((uint64_t)ts.tv_sec) + NS_TO_US((uint64_t)ts.tv_nsec);
    return us;
}

/// Get a time stamp in nanoseconds.
static uint64_t nanos()
{
    struct timespec ts;
    clock_gettime(CLOCK_MODE, &ts);
    uint64_t ns = SEC_TO_NS((uint64_t)ts.tv_sec) + (uint64_t)ts.tv_nsec;
    return ns;
}

static int c4m_time () { return millis(); }

// open()'s flags, as this host numbers them, for c4m_oflags() in c4m.c:
// every guest is compiled with Linux's numbers (include/fcntl.h), and
// the VM's OPEN hands them to the host. Windows numbers them otherwise,
// under MSYS2 or Cygwin as under MinGW, and MinGW opens in text mode
// unless asked for O_BINARY. Set only here, a header plain c4 never
// reads; without C4M_OHOST, c4m.c declares them itself, zero, which is
// "Linux, as they are". Static: several of c4mp's modules include this.
#include <fcntl.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#if O_CREAT != 0100 || O_BINARY
#define C4M_OHOST 1
static int c4m_ohost = 1;
static int c4m_o_creat = O_CREAT, c4m_o_excl = O_EXCL, c4m_o_trunc = O_TRUNC,
    c4m_o_append = O_APPEND, c4m_o_nonblock = O_NONBLOCK, c4m_o_binary = O_BINARY;
// and a file the guest creates gets a mode: the VM's OPEN passes none,
// and what is left where the third argument would be is, on Windows, a
// mode that can leave the file unreadable. A third argument already
// there is the one open() reads; this one then goes unread.
#define open(path, ...) open(path, __VA_ARGS__, 0666)
#endif

// NB: for all 3 timestamp functions above: gcc defines the type of the internal
// `tv_sec` seconds value inside the `struct timespec`, which is used
// internally in these functions, as a signed `long int`. For architectures
// where `long int` is 64 bits, that means it will have undefined
// (signed) overflow in 2^64 sec = 5.8455 x 10^11 years. For architectures
// where this type is 32 bits, it will occur in 2^32 sec = 136 years. If the
// implementation-defined epoch for the timespec is 1970, then your program
// could have undefined behavior signed time rollover in as little as
// 136 years - (year 2021 - year 1970) = 136 - 51 = 85 years. If the epoch
// was 1900 then it could be as short as 136 - (2021 - 1900) = 136 - 121 =
// 15 years. Hopefully your program won't need to run that long. :). To see,
// by inspection, what your system's epoch is, simply print out a timestamp and
// calculate how far back a timestamp of 0 would have occurred. Ex: convert
// the timestamp to years and subtract that number of years from the present
// year.
#endif // ifndef __C4M_UTIL_C 
#endif // ifndef C4CC
