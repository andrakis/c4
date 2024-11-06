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

/// Get a time stamp in milliseconds.
uint64_t millis()
{
    struct timespec ts;
    clock_gettime(CLOCK_MODE, &ts);
    uint64_t ms = SEC_TO_MS((uint64_t)ts.tv_sec) + NS_TO_MS((uint64_t)ts.tv_nsec);
    return ms;
}

/// Get a time stamp in microseconds.
uint64_t micros()
{
    struct timespec ts;
    clock_gettime(CLOCK_MODE, &ts);
    uint64_t us = SEC_TO_US((uint64_t)ts.tv_sec) + NS_TO_US((uint64_t)ts.tv_nsec);
    return us;
}

/// Get a time stamp in nanoseconds.
uint64_t nanos()
{
    struct timespec ts;
    clock_gettime(CLOCK_MODE, &ts);
    uint64_t ns = SEC_TO_NS((uint64_t)ts.tv_sec) + (uint64_t)ts.tv_nsec;
    return ns;
}

int c4m_time () { return millis(); }

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
