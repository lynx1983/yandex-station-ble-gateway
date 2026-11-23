#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "activity.h"
#include "config.h"
#include "job.h"
#include "listener.h"
#include "quasar.h"
#include "util.h"
#include "voice.h"

#ifndef SESSION_TIMEOUT_MS
#define SESSION_TIMEOUT_MS DEFAULT_SESSION_TIMEOUT_MS
#endif
#ifndef SPEECH_TIMEOUT_MS
#define SPEECH_TIMEOUT_MS DEFAULT_SPEECH_TIMEOUT_MS
#endif

volatile sig_atomic_t stopping;

static long long session_timeout_ms(void)
{
    return config_ms("KETTLE_SESSION_TIMEOUT_MS", SESSION_TIMEOUT_MS);
}

static long long speech_timeout_ms(void)
{
    return config_ms("KETTLE_SPEECH_TIMEOUT_MS", SPEECH_TIMEOUT_MS);
}

void stop_listener(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

int listen_for_commands(int fd, const char *worker, const struct aliases *aliases,
                        int continuous)
{
    uint8_t buffer[FRAME_LIMIT + 4];
    size_t length = 0;
    struct voice voice = {0};
    struct job job = {.fd = -1};
    long long deadline = now() + session_timeout_ms();
    int result = 1;
    long long admitted_at = 0;
    long long speech_deadline = 0;
    long long reply_at = 0;
    int speaking = 0;
    int pairing = 0;
    int resume_after_pair = 0;
    unsigned speech_mode = UINT32_MAX;
    const char *activity_pattern = getenv("KETTLE_ACTIVITY_PATTERN");
    pid_t activity_led = 0;
    int activity_pending = 0;
    while (!stopping) {
        if (speech_deadline && now() >= speech_deadline) {
            if (!resume_after_pair && !continuous)
                break;
            if (!speaking)
                fprintf(stderr, "Quasar speaking state was not observed\n");
            speech_deadline = 0;
            pairing = 0;
            if (resume_after_pair)
                fprintf(stderr, "Pairing complete; listening for a new command\n");
            resume_after_pair = 0;
            voice.armed = 0;
            voice.previous_started = 0;
            deadline = now() + session_timeout_ms();
        }
        if (job.pid && job.deadline && now() >= job.deadline) {
            fprintf(stderr, "Kettle job exceeded its time limit: %s\n",
                    operation_names[job.operation]);
            activity_pending = 0;
            stop_activity_led(&activity_led);
            job.deadline = 0;
            job.timed_out = 1;
            reply_at = now();
            terminate_job(job.pid);
        }
        if (!continuous && !speech_deadline && now() >= deadline && !job.pid)
            break;
        struct pollfd fds[] = {{fd, POLLIN, 0}, {reply_at ? -1 : job.fd, POLLIN, 0}};
        long long until_reply = reply_at ? reply_at - now() : 500;
        int wait_ms = until_reply <= 0 ? 0 : until_reply < 500 ? (int)until_reply : 500;
        int ready = poll(fds, 2, wait_ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            break;
        if (fds[0].revents) {
            ssize_t received = recv(fd, buffer + length, sizeof(buffer) - length, 0);
            if (received <= 0)
                break;
            length += received;
            size_t consumed = 0;
            while (length - consumed >= 4) {
                const uint8_t *header = buffer + consumed;
                uint32_t size = (uint32_t)header[0] | (uint32_t)header[1] << 8 |
                                (uint32_t)header[2] << 16 | (uint32_t)header[3] << 24;
                if (size > FRAME_LIMIT)
                    goto cleanup;
                if (length - consumed - 4 < size)
                    break;
                struct request request = decode((struct bytes){header + 4, size});
                if (request.present && request.mode != 0)
                    stop_activity_led(&activity_led);
                if (activity_pending && request.present && request.mode == 0) {
                    activity_pending = 0;
                    activity_led = start_activity_led(activity_pattern, fd, job.fd);
                }
                if (speech_deadline && request.present && request.mode != speech_mode) {
                    speech_mode = request.mode;
                    if (speech_mode == 2)
                        speaking = 1;
                    fprintf(stderr, "Quasar speech state=%u; elapsed_ms=%lld\n",
                            speech_mode, now() - admitted_at);
                    if ((continuous || resume_after_pair) && speaking && speech_mode == 0) {
                        speech_deadline = 0;
                        pairing = 0;
                        voice.armed = 0;
                        voice.previous_started = 0;
                        deadline = now() + session_timeout_ms();
                        if (resume_after_pair)
                            fprintf(stderr, "Pairing complete; listening for a new command\n");
                        resume_after_pair = 0;
                    }
                }
                if (continuous && speech_deadline && !pairing &&
                    request.present && request.mode == 1 && !request.final) {
                    if (!speaking)
                        fprintf(stderr, "Previous reply did not reach Quasar speaking state\n");
                    speech_deadline = 0;
                    voice.armed = 0;
                    voice.previous_started = 0;
                }
                int target = 0;
                enum operation operation = (speech_deadline || pairing) ? NO_OPERATION : admit(&voice, request, &job, aliases, &target);
                int invalid_heat = operation == START_HEAT && target == 0;
                if (operation && (invalid_heat ||
                    start_job(&job, worker, fd, request.wrapped, operation, target) == 0)) {
                    admitted_at = now();
                    activity_pending = activity_pattern && activity_pattern[0] == '/';
                    fprintf(stderr, "Kettle request accepted: %s\n", operation_names[operation]);
                    if (send_method(fd, request.wrapped, 6, NULL) < 0 ||
                        send_method(fd, request.wrapped, 14, NULL) < 0)
                        goto cleanup;
                    fprintf(stderr, "Cancellation sent; elapsed_ms=%lld\n", now() - admitted_at);
                    if (invalid_heat) {
                        fprintf(stderr, "Rejected heat request with invalid target\n");
                        if (send_method(fd, request.wrapped, 18,
                            "Задайте целую температуру от 35 до 90 градусов.") < 0)
                            goto cleanup;
                        result = 0;
                        speaking = 0;
                        speech_mode = UINT32_MAX;
                        speech_deadline = now() + speech_timeout_ms();
                    }
                }
                consumed += 4 + size;
            }
            memmove(buffer, buffer + consumed, length - consumed);
            length -= consumed;
        }
        if (fds[1].revents) {
            char chunk[32];
            ssize_t count = read(job.fd, chunk, sizeof(chunk));
            if (count > 0 && job.length + (size_t)count < sizeof(job.output)) {
                memcpy(job.output + job.length, chunk, count);
                job.length += count;
            } else if (count > 0) {
                job.output[0] = '\0';
                job.length = sizeof(job.output) - 1;
            } else if (count == 0) {
                activity_pending = 0;
                stop_activity_led(&activity_led);
                reply_at = now();
                if (job.operation == STOP_HEATING && reply_at < admitted_at + 900)
                    reply_at = admitted_at + 900;
            } else if (errno != EINTR) {
                break;
            }
        }
        if (reply_at && now() >= reply_at) {
            reply_at = 0;
            int stale = job.stale;
            int reply = finish_job(&job, fd);
            result = reply < 0 ? 1 : 0;
            if (result || (stale && !continuous))
                break;
            if (stale) {
                pairing = 0;
                continue;
            }
            fprintf(stderr, "Vocalize directive sent; elapsed_ms=%lld\n", now() - admitted_at);
            if (reply == 1) {
                if (start_job(&job, worker, fd, job.wrapped, PAIR_KEY, 0) < 0) {
                    result = 1;
                    break;
                }
                pairing = 1;
            } else {
                if (reply == 2) {
                    unsetenv("KETTLE_KEY_AVAILABLE");
                    resume_after_pair = 1;
                }
                speaking = 0;
                speech_mode = UINT32_MAX;
                speech_deadline = now() + speech_timeout_ms();
            }
        }
    }
cleanup:
    stop_activity_led(&activity_led);
    if (speech_deadline && !speaking)
        fprintf(stderr, "Quasar speaking state was not observed\n");
    if (job.pid)
        stop_job(job.pid);
    if (continuous)
        return stopping ? 0 : 1;
    return result;
}
