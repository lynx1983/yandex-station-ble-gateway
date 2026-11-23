#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "job.h"
#include "phrases.h"
#include "quasar.h"
#include "util.h"
#include "voice.h"

static struct bytes span(const char *text)
{
    return (struct bytes){(const uint8_t *)text, strlen(text)};
}

static void put_varint(uint8_t *out, size_t *length, uint64_t value)
{
    while (value >= 128) {
        out[(*length)++] = (value & 127) | 128;
        value >>= 7;
    }
    out[(*length)++] = (uint8_t)value;
}

static void put_bytes(uint8_t *out, size_t *length, unsigned number, struct bytes body)
{
    put_varint(out, length, number * 8 + 2);
    put_varint(out, length, body.len);
    memcpy(out + *length, body.data, body.len);
    *length += body.len;
}

static void put_value(uint8_t *out, size_t *length, unsigned number, uint64_t value)
{
    put_varint(out, length, number * 8);
    put_varint(out, length, value);
}

static size_t recognition_frame(uint8_t *out, const char *text, const char *id,
                                int final, unsigned marker)
{
    uint8_t result[768], state[1024];
    size_t r = 0, s = 0, n = 0;
    put_bytes(result, &r, 1, span(text));
    if (final)
        put_value(result, &r, 2, 1);
    put_value(state, &s, 1, 1);
    put_bytes(state, &s, 7, span(id));
    put_bytes(state, &s, 15, (struct bytes){result, r});
    if (marker) {
        put_bytes(out, &n, 252, (struct bytes){state, s});
        put_value(out, &n, 253, marker);
    } else {
        put_bytes(out, &n, 3, (struct bytes){state, s});
    }
    return n;
}

struct test_field {
    unsigned number, wire;
    uint64_t value;
    struct bytes body;
};

static int read_varint(struct bytes *input, uint64_t *value)
{
    *value = 0;
    for (unsigned shift = 0; shift < 64 && input->len; shift += 7) {
        uint8_t byte = *input->data++;
        input->len--;
        *value |= (uint64_t)(byte & 127) << shift;
        if (byte < 128)
            return 1;
    }
    return 0;
}

static int next_field(struct bytes *input, struct test_field *field)
{
    memset(field, 0, sizeof(*field));
    uint64_t key;
    if (!read_varint(input, &key) || !key)
        return 0;
    field->number = (unsigned)(key >> 3);
    field->wire = (unsigned)(key & 7);
    uint64_t length = 0;
    switch (field->wire) {
    case 0:
        if (!read_varint(input, &field->value))
            return 0;
        break;
    case 1:
        length = 8;
        break;
    case 2:
        if (!read_varint(input, &length))
            return 0;
        break;
    case 5:
        length = 4;
        break;
    default:
        return 0;
    }
    if (length > input->len)
        return 0;
    field->body = (struct bytes){input->data, (size_t)length};
    input->data += length;
    input->len -= (size_t)length;
    return 1;
}

static struct test_field find_field(struct bytes input, unsigned number, unsigned wire)
{
    struct test_field field;
    while (next_field(&input, &field))
        if (field.number == number && field.wire == wire)
            return field;
    return (struct test_field){0};
}

static void test_config_and_job_timeouts(void)
{
    setenv("KETTLE_TEST_TIMEOUT_MS", "2500", 1);
    assert(config_ms("KETTLE_TEST_TIMEOUT_MS", 1000) == 2500);
    setenv("KETTLE_TEST_TIMEOUT_MS", "abc", 1);
    assert(config_ms("KETTLE_TEST_TIMEOUT_MS", 1000) == 1000);
    setenv("KETTLE_TEST_TIMEOUT_MS", "0", 1);
    assert(config_ms("KETTLE_TEST_TIMEOUT_MS", 1000) == 1000);
    unsetenv("KETTLE_TEST_TIMEOUT_MS");
    assert(config_ms("KETTLE_TEST_TIMEOUT_MS", 1000) == 1000);

    setenv("KETTLE_JOB_TIMEOUT_MS", "250", 1);
    setenv("KETTLE_STOP_GRACE_MS", "100", 1);
    long long mark = now();
    struct job job;
    assert(start_job(&job, "/bin/true", -1, 0, READ_TEMPERATURE, 0) == 0);
    long long remaining = job.deadline - mark;
    assert(remaining >= 200 && remaining <= 350);
    close(job.fd);
    stop_job(job.pid);

    setenv("KETTLE_PAIR_TIMEOUT_MS", "400", 1);
    mark = now();
    assert(start_job(&job, "/bin/true", -1, 0, PAIR_KEY, 0) == 0);
    remaining = job.deadline - mark;
    assert(remaining >= 450 && remaining <= 600);
    close(job.fd);
    stop_job(job.pid);

    unsetenv("KETTLE_JOB_TIMEOUT_MS");
    unsetenv("KETTLE_STOP_GRACE_MS");
    unsetenv("KETTLE_PAIR_TIMEOUT_MS");
}

static void test_decode(void)
{
    uint8_t frame[2048];
    const char words[] = "какая температура воды в чайнике";
    size_t length = recognition_frame(frame, words, "req-7", 1, 0);
    struct request request = decode((struct bytes){frame, length});
    assert(request.present && request.final && !request.wrapped);
    assert(request.mode == 1);
    assert(request.id.len == 5 && memcmp(request.id.data, "req-7", 5) == 0);
    assert(request.text.len == strlen(words));
    assert(memcmp(request.text.data, words, strlen(words)) == 0);

    length = recognition_frame(frame, words, "req-7", 1, 210);
    request = decode((struct bytes){frame, length});
    assert(request.present && request.final && request.wrapped);
    assert(request.text.len == strlen(words));

    length = recognition_frame(frame, words, "req-7", 1, 3);
    request = decode((struct bytes){frame, length});
    assert(request.present && request.final && request.wrapped);

    length = recognition_frame(frame, "температура", "req-8", 0, 0);
    request = decode((struct bytes){frame, length});
    assert(request.present && !request.final);
}

static void check_directive(int wrapped)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(send_method(fds[0], wrapped, wrapped ? 6 : 18,
                       wrapped ? NULL : "Привет") == 0);
    close(fds[0]);
    uint8_t buffer[1024];
    size_t total = 0;
    ssize_t count;
    while (total < sizeof(buffer) &&
           (count = read(fds[1], buffer + total, sizeof(buffer) - total)) > 0)
        total += (size_t)count;
    close(fds[1]);
    assert(total >= 4);
    uint32_t size = (uint32_t)buffer[0] | (uint32_t)buffer[1] << 8 |
                    (uint32_t)buffer[2] << 16 | (uint32_t)buffer[3] << 24;
    assert(size == total - 4);

    struct bytes body = {buffer + 4, size};
    struct test_field remoting = find_field(body, wrapped ? 252 : 210, 2);
    assert(remoting.number != 0);
    if (wrapped) {
        struct test_field marker = find_field(body, 253, 0);
        assert(marker.value == 210);
    }
    struct test_field action = find_field(remoting.body, 8, 2);
    assert(action.number != 0);
    struct test_field method = find_field(action.body, 1, 0);
    assert(method.value == (uint64_t)(wrapped ? 6 : 18));
    struct test_field vocalize = find_field(action.body, 13, 2);
    if (wrapped) {
        assert(vocalize.number == 0);
    } else {
        assert(vocalize.number != 0);
        struct test_field spoken = find_field(vocalize.body, 1, 2);
        assert(spoken.body.len == strlen("Привет"));
        assert(memcmp(spoken.body.data, "Привет", spoken.body.len) == 0);
        assert(find_field(vocalize.body, 2, 0).value == 2);
    }
}

static void test_phrases(void)
{
    int target = -1;
    assert(kettle_phrase(&default_aliases,
                         span("Какая температура воды в чайнике?"), &target) ==
           READ_TEMPERATURE);
    assert(kettle_phrase(&default_aliases, span("алиса, как там чайник"), &target) ==
           READ_STATUS);
    assert(strcmp(operation_names[START_HEAT], "heat") == 0);

    target = -1;
    assert(kettle_phrase(&default_aliases,
                         span("нагрей воду в чайнике до пятидесяти градусов"),
                         &target) == START_HEAT && target == 50);
    target = -1;
    assert(kettle_phrase(&default_aliases,
                         span("нагрей воду в чайнике до 90 °C"), &target) ==
           START_HEAT && target == 90);
    target = -1;
    assert(kettle_phrase(&default_aliases,
                         span("нагрей воду в чайнике до двадцати градусов"),
                         &target) == START_HEAT && target == 0);
    target = -1;
    assert(kettle_phrase(&default_aliases,
                         span("нагрей воду в чайнике для термоса"), &target) ==
           START_HEAT && target == 75);
    assert(kettle_phrase(&default_aliases, span("какая погода"), &target) ==
           NO_OPERATION);

    char path[] = "/tmp/kettle-phrases-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const char config[] = "temperature=какая погода\nheat.75=нагрей для термоса\n";
    assert(write(fd, config, strlen(config)) == (ssize_t)strlen(config));
    close(fd);
    struct aliases custom = {0};
    assert(load_aliases(path, &custom));
    assert(kettle_phrase(&custom, span("какая погода"), &target) == READ_TEMPERATURE);
    target = -1;
    assert(kettle_phrase(&custom, span("нагрей для термоса"), &target) ==
           START_HEAT && target == 75);
    unlink(path);

    fd = mkstemp(path);
    assert(fd >= 0);
    const char duplicate[] = "temperature=какая погода\ntemperature=какая погода\n";
    assert(write(fd, duplicate, strlen(duplicate)) == (ssize_t)strlen(duplicate));
    close(fd);
    struct aliases invalid = {0};
    assert(!load_aliases(path, &invalid));
    unlink(path);
}

static void test_voice(void)
{
    struct voice voice = {0};
    struct job job = {0};
    int target = 0;
    struct request partial = {.present = 1, .mode = 1};
    assert(admit(&voice, partial, &job, &default_aliases, &target) == NO_OPERATION);
    assert(voice.armed);

    struct request first = {.present = 1, .mode = 1, .final = 1,
                            .text = span("какая температура воды в чайнике"),
                            .id = span("r1")};
    assert(admit(&voice, first, &job, &default_aliases, &target) == READ_TEMPERATURE);
    assert(admit(&voice, first, &job, &default_aliases, &target) == NO_OPERATION);

    job.pid = 123;
    assert(admit(&voice, partial, &job, &default_aliases, &target) == NO_OPERATION);
    assert(job.stale);
    job.stale = 0;
    job.pid = 0;
    assert(admit(&voice, partial, &job, &default_aliases, &target) == NO_OPERATION);

    struct request second = {.present = 1, .mode = 1, .final = 1,
                             .text = span("какая температура воды в чайнике"),
                             .id = span("r2")};
    assert(admit(&voice, second, &job, &default_aliases, &target) == READ_TEMPERATURE);
}

int main(void)
{
    test_config_and_job_timeouts();
    test_decode();
    check_directive(0);
    check_directive(1);
    test_phrases();
    test_voice();
    puts("module tests passed");
    return 0;
}
