#define _POSIX_C_SOURCE 200809L
#include <string.h>

#include "voice.h"

enum operation admit(struct voice *voice, struct request request, struct job *job,
                     const struct aliases *aliases, int *target)
{
    if (!request.present)
        return 0;
    int fresh = !request.final && (request.mode == 1 ||
                 (request.started && !voice->previous_started));
    voice->previous_started = request.started;
    if (fresh && !voice->armed) {
        voice->armed = 1;
        if (job->pid)
            job->stale = 1;
    }
    if (!request.mode) {
        voice->armed = 0;
        return 0;
    }
    if (!request.final || !request.text.len)
        return 0;
    int armed = voice->armed;
    voice->armed = 0;
    enum operation operation = kettle_phrase(aliases, request.text, target);
    if (!armed || !operation || job->pid || request.id.len > 128)
        return 0;
    if (request.id.len) {
        for (unsigned i = 0; i < 16; i++)
            if (voice->lengths[i] == request.id.len &&
                memcmp(voice->ids[i], request.id.data, request.id.len) == 0)
                return 0;
        unsigned index = voice->next_id++ % 16;
        voice->lengths[index] = request.id.len;
        memcpy(voice->ids[index], request.id.data, request.id.len);
    }
    return operation;
}
