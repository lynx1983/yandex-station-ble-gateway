#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job.h"
#include "quasar.h"
#include "util.h"

#ifndef JOB_TIMEOUT_MS
#define JOB_TIMEOUT_MS 60000
#endif
#ifndef PAIR_TIMEOUT_MS
#define PAIR_TIMEOUT_MS 75000
#endif
#ifndef JOB_STOP_GRACE_MS
#define JOB_STOP_GRACE_MS 1000
#endif

int start_job(struct job *job, const char *worker, int fd, int wrapped,
              enum operation operation, int target)
{
    int pipes[2];
    if (pipe(pipes) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        /* A worker owns its process group so a timeout can stop its probes. */
        setpgid(0, 0);
        close(fd);
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(pipes[1]);
        if (operation == START_HEAT) {
            char argument[16];
            snprintf(argument, sizeof(argument), "%d", target);
            execl(worker, worker, operation_names[operation], argument, (char *)NULL);
        } else {
            execl(worker, worker, operation_names[operation], (char *)NULL);
        }
        _exit(126);
    }
    close(pipes[1]);
    if (pid < 0) {
        close(pipes[0]);
        return -1;
    }
    /* The child also does this; either side winning the race is enough. */
    (void)setpgid(pid, pid);
    long long timeout = operation == PAIR_KEY ? PAIR_TIMEOUT_MS : JOB_TIMEOUT_MS;
    *job = (struct job){.pid = pid, .fd = pipes[0], .wrapped = wrapped,
                        .operation = operation, .target = target,
                        .deadline = now() + timeout};
    return 0;
}

static void signal_job(pid_t pid, int signal_number)
{
    if (pid <= 0)
        return;
    /* A timeout must stop the probe and any children holding its stdout. */
    if (kill(-pid, signal_number) < 0)
        kill(pid, signal_number);
}

void terminate_job(pid_t pid)
{
    signal_job(pid, SIGTERM);
}

static pid_t wait_job(pid_t pid, int *status, int bounded)
{
    long long deadline = bounded ? now() + JOB_STOP_GRACE_MS : 0;
    pid_t waited;
    for (;;) {
        waited = waitpid(pid, status, bounded ? WNOHANG : 0);
        if (waited == pid || (waited < 0 && errno != EINTR))
            break;
        if (waited == 0 && (!bounded || now() >= deadline))
            break;
        const struct timespec pause = {.tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
    return waited;
}

void stop_job(pid_t pid)
{
    if (pid <= 0)
        return;
    terminate_job(pid);
    int status;
    if (wait_job(pid, &status, 1) == 0) {
        signal_job(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
}

int finish_job(struct job *job, int fd)
{
    int status = 0;
    close(job->fd);
    job->fd = -1;
    pid_t waited = wait_job(job->pid, &status, 1);
    if (waited == 0) {
        signal_job(job->pid, SIGKILL);
        do {
            waited = waitpid(job->pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    job->pid = 0;
    job->deadline = 0;
    int stale = job->stale;
    int timed_out = job->timed_out;
    job->stale = 0;
    job->timed_out = 0;
    if (stale)
        return 0;
    if (timed_out) {
        fprintf(stderr, "Kettle job timed out: %s\n", operation_names[job->operation]);
        const char *message = job->operation == PAIR_KEY
            ? "Привязка чайника не завершилась вовремя. Попробуйте ещё раз."
            : "Не удалось дождаться ответа от чайника. Попробуйте ещё раз.";
        return send_method(fd, job->wrapped, 18, message);
    }
    if (waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 21)
        return send_method(fd, job->wrapped, 18,
                           "Укажите адрес чайника в настройках службы.");
    if (waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 20) {
        fprintf(stderr, "Kettle key missing\n");
        if (!getenv("KETTLE_PAIR_KEY"))
            return send_method(fd, job->wrapped, 18,
                "Ключ чайника не настроен. Запустите привязку чайника.");
        int sent = send_method(fd, job->wrapped, 18,
            "Чайник не подключен, давайте его подключим. "
            "В режиме ожидания удерживайте кнопку чайника до трёх коротких сигналов.");
        return sent < 0 ? sent : 1;
    }
    int success = waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (job->operation == PAIR_KEY) {
        fprintf(stderr, "Voice pairing %s\n", success ? "verified" : "failed");
        int sent = send_method(fd, job->wrapped, 18, success
            ? "Чайник привязан. Повторите нужную команду."
            : "Не удалось привязать чайник. Попробуйте ещё раз.");
        return sent == 0 && success ? 2 : sent;
    }
    if (job->operation == STOP_HEATING) {
        const char *reply = "Не удалось подтвердить остановку чайника.";
        if (success && strcmp(job->output, "stopped\n") == 0)
            reply = "Нагрев чайника остановлен.";
        else if (success && strcmp(job->output, "idle\n") == 0)
            reply = "Чайник сейчас не нагревает воду.";
        fprintf(stderr, "Stop result: %s\n", reply);
        return send_method(fd, job->wrapped, 18, reply);
    }
    if (job->operation == START_HEAT) {
        char reply[192];
        const char *message = "Не удалось подтвердить запуск нагрева.";
        if (success && strcmp(job->output, "busy\n") == 0)
            message = "Чайник уже работает.";
        else if (success && strcmp(job->output, "reached\n") == 0)
            message = "Вода уже достигла заданной температуры.";
        else if (success && strcmp(job->output, "started\n") == 0) {
            snprintf(reply, sizeof(reply), "Чайник начал нагрев до %d °C.", job->target);
            message = reply;
        }
        fprintf(stderr, "Heat result: target=%d; %s\n", job->target, message);
        return send_method(fd, job->wrapped, 18, message);
    }
    if (job->operation == START_BOIL) {
        const char *reply = "Не удалось подтвердить запуск кипячения.";
        if (success && strcmp(job->output, "started\n") == 0)
            reply = "Чайник начал кипятить воду.";
        else if (success && strcmp(job->output, "busy\n") == 0)
            reply = "Чайник уже работает.";
        fprintf(stderr, "Boil result: %s\n", reply);
        return send_method(fd, job->wrapped, 18, reply);
    }
    char reply[256];
    const char *reading = job->output;
    int heating = -1;
    if (job->operation == READ_STATUS) {
        if ((reading[0] == '0' || reading[0] == '1') && reading[1] == ' ') {
            heating = reading[0] - '0';
            reading += 2;
        } else {
            success = 0;
        }
    }
    char *end;
    long temperature = strtol(reading, &end, 10);
    int valid_temperature = end != reading && temperature >= 0 && temperature <= 100;
    long target = 0;
    if (heating == 1 && *end == ' ') {
        const char *target_reading = end + 1;
        target = strtol(target_reading, &end, 10);
        if (end == target_reading || target < 1 || target > 100)
            target = 0;
    }
    if (success && valid_temperature && (*end == '\n' || *end == '\0')) {
        if (job->operation == READ_STATUS) {
            if (heating && target)
                snprintf(reply, sizeof(reply),
                         "Чайник нагревает воду до %ld °C. Сейчас примерно %ld °C.",
                         target, temperature);
            else
                snprintf(reply, sizeof(reply), "Чайник %s. Вода примерно %ld °C.",
                         heating ? "нагревает воду" : "сейчас не нагревает воду", temperature);
            fprintf(stderr, "Kettle status: heating=%d temperature=%ld C target=%ld C\n",
                    heating, temperature, target);
        } else {
            snprintf(reply, sizeof(reply), "Температура воды в чайнике — %ld °C.", temperature);
            fprintf(stderr, "Temperature read: %ld C\n", temperature);
        }
    } else {
        snprintf(reply, sizeof(reply), "Не удалось получить температуру воды в чайнике.");
        fprintf(stderr, "Temperature read failed\n");
    }
    return send_method(fd, job->wrapped, 18, reply);
}
