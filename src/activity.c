#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "activity.h"

pid_t start_activity_led(const char *pattern, int socket_fd, int worker_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        close(socket_fd);
        close(worker_fd);
        if (chdir("/system/vendor/quasar") == 0)
            execl("./led_util", "./led_util", pattern, (char *)NULL);
        _exit(126);
    }
    return pid;
}

void stop_activity_led(pid_t *pid)
{
    if (*pid <= 0)
        return;
    kill(*pid, SIGTERM);
    const struct timespec pause = {.tv_nsec = 10000000};
    for (int attempt = 0; attempt < 20; attempt++) {
        if (waitpid(*pid, NULL, WNOHANG) == *pid) {
            *pid = 0;
            return;
        }
        nanosleep(&pause, NULL);
    }
    kill(*pid, SIGKILL);
    while (waitpid(*pid, NULL, 0) < 0 && errno == EINTR) {}
    *pid = 0;
}
