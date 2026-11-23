#ifndef QUASAR_H
#define QUASAR_H

#include <stddef.h>
#include <stdint.h>

#define FRAME_LIMIT 65536

struct bytes { const uint8_t *data; size_t len; };
struct field { unsigned number, wire; uint64_t value; struct bytes body; };
struct request {
    unsigned mode;
    int present, final, started, wrapped;
    struct bytes text, id;
};

struct request decode(struct bytes frame);
int send_method(int fd, int wrapped, unsigned method, const char *text);

#endif
