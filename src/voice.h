#ifndef VOICE_H
#define VOICE_H

#include <stdint.h>

#include "job.h"
#include "phrases.h"
#include "quasar.h"

struct voice {
    int armed, previous_started;
    unsigned next_id;
    uint8_t ids[16][128];
    size_t lengths[16];
};

enum operation admit(struct voice *voice, struct request request, struct job *job,
                     const struct aliases *aliases, int *target);

#endif
