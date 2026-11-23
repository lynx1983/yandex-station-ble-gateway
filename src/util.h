#ifndef UTIL_H
#define UTIL_H

#include <time.h>

static inline long long now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#endif
