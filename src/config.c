#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdlib.h>

#include "config.h"

long long config_ms(const char *name, long long fallback)
{
    const char *raw = getenv(name);
    if (!raw || !*raw)
        return fallback;
    errno = 0;
    char *end = NULL;
    long long value = strtoll(raw, &end, 10);
    if (errno || end == raw || *end || value <= 0 || value > 3600000)
        return fallback;
    return value;
}
