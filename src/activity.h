#ifndef ACTIVITY_H
#define ACTIVITY_H

#include <sys/types.h>

pid_t start_activity_led(const char *pattern, int socket_fd, int worker_fd);
void stop_activity_led(pid_t *pid);

#endif
