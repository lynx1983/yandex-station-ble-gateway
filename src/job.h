#ifndef JOB_H
#define JOB_H

#include <sys/types.h>

#include "phrases.h"

struct job {
    pid_t pid;
    int fd, wrapped, stale, target, timed_out;
    enum operation operation;
    size_t length;
    long long deadline;
    char output[32];
};

int start_job(struct job *job, const char *worker, int fd, int wrapped,
              enum operation operation, int target);
int finish_job(struct job *job, int fd);
void terminate_job(pid_t pid);
void stop_job(pid_t pid);

#endif
