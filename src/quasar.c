#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "quasar.h"

static int varint(struct bytes *input, uint64_t *value)
{
    *value = 0;
    for (unsigned shift = 0; shift < 64 && input->len; shift += 7) {
        unsigned byte = *input->data++;
        input->len--;
        if (shift == 63 && byte > 1)
            return 0;
        *value |= (uint64_t)(byte & 127) << shift;
        if (byte < 128)
            return 1;
    }
    return 0;
}

static struct field find(struct bytes input, unsigned number, unsigned wire)
{
    while (input.len) {
        uint64_t key, length = 0;
        struct field field = {0};
        if (!varint(&input, &key) || key == 0)
            break;
        field.number = key >> 3;
        field.wire = key & 7;
        switch (field.wire) {
        case 0: if (!varint(&input, &field.value)) return (struct field){0}; break;
        case 1: length = 8; break;
        case 2: if (!varint(&input, &length)) return (struct field){0}; break;
        case 5: length = 4; break;
        default: return (struct field){0};
        }
        if (length > input.len)
            break;
        field.body = (struct bytes){input.data, length};
        if (field.number == number && field.wire == wire)
            return field;
        input.data += length;
        input.len -= length;
    }
    return (struct field){0};
}

struct request decode(struct bytes frame)
{
    struct request request = {0};
    struct bytes state = find(frame, 3, 2).body;
    /* Observed wrappers use either the Alice marker or the reply marker. */
    uint64_t marker = find(frame, 253, 0).value;
    if (!state.data && (marker == 3 || marker == 210)) {
        request.wrapped = 1;
        state = find(frame, 252, 2).body;
    }
    struct field mode = find(state, 1, 0);
    if (!mode.number)
        return request;
    request.present = 1;
    request.mode = mode.value;
    request.id = find(state, 7, 2).body;
    struct bytes result = find(state, 15, 2).body;
    if (result.data) {
        request.final = find(result, 2, 0).value == 1;
        request.started = find(result, 4, 0).value == 1;
    } else {
        result = find(state, 13, 2).body;
        request.final = find(result, 4, 0).value == 1;
    }
    request.text = find(result, 1, 2).body;
    return request;
}

static void put_varint(uint8_t *out, size_t *length, unsigned value)
{
    while (value >= 128) {
        out[(*length)++] = (value & 127) | 128;
        value >>= 7;
    }
    out[(*length)++] = value;
}

static void put_bytes(uint8_t *out, size_t *length, unsigned tag, const void *data, size_t n)
{
    put_varint(out, length, tag * 8 + 2);
    put_varint(out, length, n);
    memcpy(out + *length, data, n);
    *length += n;
}

int send_method(int fd, int wrapped, unsigned method, const char *text)
{
    enum { MAX_VOCALIZE_TEXT = 256 };
    uint8_t vocalize[MAX_VOCALIZE_TEXT + 8], action[MAX_VOCALIZE_TEXT + 16];
    uint8_t remoting[MAX_VOCALIZE_TEXT + 40], frame[MAX_VOCALIZE_TEXT + 56];
    size_t v = 0, a = 0, r = 0, n = 4;
    put_varint(action, &a, 8);
    put_varint(action, &a, method);
    if (text) {
        if (strlen(text) > MAX_VOCALIZE_TEXT)
            return -1;
        put_bytes(vocalize, &v, 1, text, strlen(text));
        put_varint(vocalize, &v, 16);
        put_varint(vocalize, &v, 2);
        put_bytes(action, &a, 13, vocalize, v);
    }
    put_bytes(remoting, &r, 7, "AliceCapability", 15);
    put_bytes(remoting, &r, 8, action, a);
    put_bytes(frame, &n, wrapped ? 252 : 210, remoting, r);
    if (wrapped) {
        put_varint(frame, &n, 253 * 8);
        put_varint(frame, &n, 210);
    }
    size_t body = n - 4;
    for (unsigned i = 0; i < 4; i++)
        frame[i] = body >> (i * 8);
    for (size_t offset = 0; offset < n;) {
        ssize_t wrote = send(fd, frame + offset, n - offset, MSG_NOSIGNAL);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return -1;
        offset += wrote;
    }
    return 0;
}
