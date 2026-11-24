#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "listener.h"
#include "phrases.h"

#ifndef QUASAR_PORT
#define QUASAR_PORT 9875
#endif

int main(int argc, char **argv)
{
    int continuous = argc > 1 && strcmp(argv[1], "--continuous") == 0;
    if (continuous) {
        argc--;
        argv++;
        struct sigaction action = {.sa_handler = stop_listener};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, NULL) < 0 || sigaction(SIGINT, &action, NULL) < 0)
            return 1;
    }
    if (argc != 3 || access(argv[1], X_OK) < 0) {
        fprintf(stderr, "Usage: alice-kettle [--continuous] WORKER PHRASE_FILE\n");
        return 2;
    }
    struct aliases configured = {0};
    if (!load_aliases(argv[2], &configured))
        return 2;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in endpoint = {
        .sin_family = AF_INET, .sin_port = htons(QUASAR_PORT),
        .sin_addr.s_addr = htonl(0x7f000001u)
    };
    if (fd < 0 || connect(fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) < 0) {
        perror("Quasar connection");
        return 1;
    }
    fprintf(stderr, "Listening for kettle voice commands\n");
    int result = listen_for_commands(fd, argv[1], &configured, continuous);
    close(fd);
    return result;
}
