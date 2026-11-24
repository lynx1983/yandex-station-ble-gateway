#ifndef PHRASES_H
#define PHRASES_H

#include <stddef.h>

#include "quasar.h"

enum operation {
    NO_OPERATION,
    READ_TEMPERATURE,
    STOP_HEATING,
    READ_STATUS,
    START_BOIL,
    START_HEAT,
    PAIR_KEY
};

#define PHRASE_SIZE 256
#define PHRASE_LIMIT 64

struct alias {
    enum operation operation;
    char phrase[PHRASE_SIZE];
    int target;
};
struct aliases {
    struct alias entries[PHRASE_LIMIT];
    size_t count;
};

extern const char *const operation_names[];

enum operation kettle_phrase(const struct aliases *aliases, struct bytes text, int *target);
int load_aliases(const char *path, struct aliases *aliases);

#endif
