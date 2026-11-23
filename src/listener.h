#ifndef LISTENER_H
#define LISTENER_H

#include <signal.h>

#include "phrases.h"

extern volatile sig_atomic_t stopping;

void stop_listener(int signal_number);
int listen_for_commands(int fd, const char *worker, const struct aliases *aliases,
                        int continuous);

#endif
