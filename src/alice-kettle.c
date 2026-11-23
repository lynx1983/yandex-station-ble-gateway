#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct bytes { const uint8_t *data; size_t len; };
struct field { unsigned number, wire; uint64_t value; struct bytes body; };

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

struct request {
    unsigned mode;
    int present, final, started, wrapped;
    struct bytes text, id;
};

static struct request decode(struct bytes frame)
{
    struct request request = {0};
    struct bytes state = find(frame, 3, 2).body;
    if (!state.data && find(frame, 253, 0).value == 3) {
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

enum operation { NO_OPERATION, READ_TEMPERATURE, STOP_HEATING, READ_STATUS, START_BOIL, START_HEAT, PAIR_KEY };

#define PHRASE_SIZE 256
#define PHRASE_LIMIT 64

struct alias { enum operation operation; char phrase[PHRASE_SIZE]; int target; };
struct aliases { struct alias entries[PHRASE_LIMIT]; size_t count; };

static const struct aliases defaults = {
    .entries = {
        {READ_TEMPERATURE, "какая температура воды в чайнике", 0},
        {READ_STATUS, "какой статус чайника", 0},
        {READ_STATUS, "как там чайник", 0},
        {STOP_HEATING, "останови кипячение", 0},
        {STOP_HEATING, "прекрати нагрев чайника", 0},
        {START_BOIL, "вскипяти воду в чайнике", 0},
    },
    .count = 6,
};

static const char *const operation_names[] = {
    [READ_TEMPERATURE] = "temperature",
    [READ_STATUS] = "status",
    [STOP_HEATING] = "stop",
    [START_BOIL] = "boil",
    [START_HEAT] = "heat",
    [PAIR_KEY] = "pair",
};

static char *trim(char *text, const char *trailing)
{
    text += strspn(text, " \t\r\n");
    size_t length = strlen(text);
    while (length && strchr(trailing, text[length - 1]))
        text[--length] = 0;
    return text;
}

static int normalize(struct bytes text, char out[PHRASE_SIZE])
{
    if (!text.len || text.len >= PHRASE_SIZE || memchr(text.data, 0, text.len))
        return 0;
    for (size_t i = 0; i < text.len; i++) {
        uint8_t byte = text.data[i];
        uint8_t next = i + 1 < text.len ? text.data[i + 1] : 0;
        if (byte >= 'A' && byte <= 'Z') {
            out[i] = byte + ('a' - 'A');
        } else if (byte == 0xd0 && next >= 0x90 && next <= 0xaf) {
            out[i++] = next <= 0x9f ? 0xd0 : 0xd1;
            out[i] = next <= 0x9f ? next + 0x20 : next - 0x20;
        } else if (byte == 0xd0 && next == 0x81) {
            out[i++] = 0xd1;
            out[i] = 0x91;
        } else {
            out[i] = byte;
        }
    }
    out[text.len] = 0;
    char *phrase = trim(out, " \t\r\n?.!");
    const char prefix[] = "алиса";
    size_t length = sizeof(prefix) - 1;
    if (strncmp(phrase, prefix, length) == 0 &&
        (phrase[length] == ',' || phrase[length] == ' ')) {
        phrase += length;
        if (*phrase == ',')
            phrase++;
        phrase = trim(phrase, " \t\r\n?.!");
    }
    memmove(out, phrase, strlen(phrase) + 1);
    return out[0] != 0;
}

static size_t number_word_length(const char *text, const char *word)
{
    size_t length = strlen(word);
    return strncmp(text, word, length) == 0 &&
           (text[length] == ' ' || text[length] == 0) ? length : 0;
}

static int heat_target(const char *text)
{
    static const char *const tens[][2] = {
        {"тридцать", "тридцати"}, {"сорок", "сорока"},
        {"пятьдесят", "пятидесяти"}, {"шестьдесят", "шестидесяти"},
        {"семьдесят", "семидесяти"}, {"восемьдесят", "восьмидесяти"},
        {"девяносто", "девяноста"},
    };
    static const char *const units[][2] = {
        {"один", "одного"}, {"два", "двух"}, {"три", "трёх"},
        {"четыре", "четырёх"}, {"пять", "пяти"}, {"шесть", "шести"},
        {"семь", "семи"}, {"восемь", "восьми"}, {"девять", "девяти"},
    };
    int target = 0;
    if (*text >= '0' && *text <= '9') {
        while (*text >= '0' && *text <= '9' && target <= 100)
            target = target * 10 + (*text++ - '0');
    } else {
        for (size_t i = 0; i < sizeof(tens) / sizeof(tens[0]); i++) {
            size_t length = number_word_length(text, tens[i][0]);
            if (!length)
                length = number_word_length(text, tens[i][1]);
            if (length) {
                target = 30 + 10 * (int)i;
                text += length;
                break;
            }
        }
        for (size_t i = 0; target && *text == ' ' && i < sizeof(units) / sizeof(units[0]); i++) {
            size_t length = number_word_length(text + 1, units[i][0]);
            if (!length)
                length = number_word_length(text + 1, units[i][1]);
            if (!length && i == 2)
                length = number_word_length(text + 1, "трех");
            if (!length && i == 3)
                length = number_word_length(text + 1, "четырех");
            if (length) {
                target += 1 + (int)i;
                text += length + 1;
                break;
            }
        }
    }
    if (*text && strcmp(text, " градусов") && strcmp(text, " градус") &&
        strcmp(text, " градуса") && strcmp(text, " °c") && strcmp(text, "°c"))
        return 0;
    return target >= 35 && target <= 90 ? target : 0;
}

static enum operation kettle_phrase(const struct aliases *aliases, struct bytes text, int *target)
{
    char phrase[PHRASE_SIZE];
    if (!normalize(text, phrase))
        return NO_OPERATION;
    int configured_preset = 0;
    for (size_t i = 0; i < aliases->count; i++) {
        const struct alias *alias = &aliases->entries[i];
        configured_preset |= alias->target != 0;
        if ((alias->operation != START_HEAT || alias->target) &&
            strcmp(phrase, alias->phrase) == 0) {
            *target = alias->target;
            return alias->operation;
        }
    }
    if (!configured_preset &&
        strcmp(phrase, "нагрей воду в чайнике для термоса") == 0) {
        *target = 75;
        return START_HEAT;
    }

    int configured_heat = 0;
    for (size_t i = 0; i < aliases->count; i++) {
        const struct alias *alias = &aliases->entries[i];
        if (alias->operation != START_HEAT || alias->target)
            continue;
        configured_heat = 1;
        size_t length = strlen(alias->phrase);
        if (strncmp(phrase, alias->phrase, length) == 0 && phrase[length] == ' ') {
            *target = heat_target(phrase + length + 1);
            return START_HEAT;
        }
    }
    static const char default_heat[] = "нагрей воду в чайнике до ";
    if (!configured_heat && strncmp(phrase, default_heat, sizeof(default_heat) - 1) == 0) {
        *target = heat_target(phrase + sizeof(default_heat) - 1);
        return START_HEAT;
    }
    return NO_OPERATION;
}

static int load_aliases(const char *path, struct aliases *aliases)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return 0;
    }
    char line[PHRASE_SIZE + 32];
    unsigned line_number = 0;
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        if (!strchr(line, '\n') && !feof(file))
            goto invalid;
        char *entry = trim(line, " \t\r\n");
        if (!*entry || *entry == '#')
            continue;
        char *phrase = strchr(entry, '=');
        if (!phrase || aliases->count == PHRASE_LIMIT)
            goto invalid;
        *phrase++ = 0;
        char *name = trim(entry, " \t");
        int target = 0;
        if (strncmp(name, "heat.", 5) == 0) {
            const char *number = name + 5;
            if (strlen(number) != 2 || number[0] < '0' || number[0] > '9' ||
                number[1] < '0' || number[1] > '9' || !(target = heat_target(number)))
                goto invalid;
            name[4] = 0;
        }
        enum operation operation = NO_OPERATION;
        for (int i = READ_TEMPERATURE; i <= START_HEAT; i++)
            if (strcmp(name, operation_names[i]) == 0)
                operation = i;
        struct alias *alias = &aliases->entries[aliases->count];
        if (!operation || !normalize((struct bytes){(const uint8_t *)phrase, strlen(phrase)}, alias->phrase))
            goto invalid;
        for (size_t i = 0; i < aliases->count; i++)
            if (strcmp(alias->phrase, aliases->entries[i].phrase) == 0)
                goto invalid;
        alias->operation = operation;
        alias->target = target;
        aliases->count++;
    }
    if (ferror(file) || !aliases->count)
        goto invalid;
    fclose(file);
    return 1;
invalid:
    fprintf(stderr, "%s:%u: invalid, empty, or duplicate phrase entry\n", path, line_number);
    fclose(file);
    return 0;
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

static int send_method(int fd, int wrapped, unsigned method, const char *text)
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

static long long now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct job {
    pid_t pid;
    int fd, wrapped, stale, target;
    enum operation operation;
    size_t length;
    char output[32];
};

static int start_job(struct job *job, const char *worker, int fd, int wrapped,
                     enum operation operation, int target)
{
    int pipes[2];
    if (pipe(pipes) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
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
    *job = (struct job){.pid = pid, .fd = pipes[0], .wrapped = wrapped,
                        .operation = operation, .target = target};
    return 0;
}

static int finish_job(struct job *job, int fd)
{
    int status;
    close(job->fd);
    job->fd = -1;
    pid_t waited;
    do {
        waited = waitpid(job->pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    job->pid = 0;
    if (job->stale)
        return 0;
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
        if (job->target == 0)
            message = "Задайте целую температуру от 35 до 90 градусов.";
        else if (success && strcmp(job->output, "busy\n") == 0)
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

#define FRAME_LIMIT 65536
struct voice {
    int armed, previous_started;
    unsigned next_id;
    uint8_t ids[16][128];
    size_t lengths[16];
};

static enum operation admit(struct voice *voice, struct request request, struct job *job,
                            const struct aliases *aliases, int *target)
{
    if (!request.present)
        return 0;
    int fresh = !request.final && (request.mode == 1 ||
                (request.started && !voice->previous_started));
    voice->previous_started = request.started;
    if (fresh && !voice->armed) {
        voice->armed = 1;
        if (job->pid)
            job->stale = 1;
    }
    if (!request.mode) {
        voice->armed = 0;
        return 0;
    }
    if (!request.final || !request.text.len)
        return 0;
    int armed = voice->armed;
    voice->armed = 0;
    enum operation operation = kettle_phrase(aliases, request.text, target);
    if (!armed || !operation || job->pid || request.id.len > 128)
        return 0;
    if (request.id.len) {
        for (unsigned i = 0; i < 16; i++)
            if (voice->lengths[i] == request.id.len &&
                memcmp(voice->ids[i], request.id.data, request.id.len) == 0)
                return 0;
        unsigned index = voice->next_id++ % 16;
        voice->lengths[index] = request.id.len;
        memcpy(voice->ids[index], request.id.data, request.id.len);
    }
    return operation;
}

static pid_t start_activity_led(const char *pattern, int socket_fd, int worker_fd)
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

static void stop_activity_led(pid_t *pid)
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

static volatile sig_atomic_t stopping;

static void stop_listener(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int listen_for_commands(int fd, const char *worker, const struct aliases *aliases,
                               int continuous)
{
    uint8_t buffer[FRAME_LIMIT + 4];
    size_t length = 0;
    struct voice voice = {0};
    struct job job = {.fd = -1};
    long long deadline = now() + 90000;
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
            deadline = now() + 90000;
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
                        deadline = now() + 90000;
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
                if (operation && start_job(&job, worker, fd, request.wrapped, operation, target) == 0) {
                    admitted_at = now();
                    activity_pending = activity_pattern && activity_pattern[0] == '/';
                    fprintf(stderr, "Kettle request accepted: %s\n", operation_names[operation]);
                    if (send_method(fd, request.wrapped, 6, NULL) < 0 ||
                        send_method(fd, request.wrapped, 14, NULL) < 0)
                        goto cleanup;
                    fprintf(stderr, "Cancellation sent; elapsed_ms=%lld\n", now() - admitted_at);
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
                speech_deadline = now() + 8000;
            }
        }
    }
cleanup:
    stop_activity_led(&activity_led);
    if (speech_deadline && !speaking)
        fprintf(stderr, "Quasar speaking state was not observed\n");
    if (job.pid) {
        close(job.fd);
        kill(job.pid, SIGTERM);
        while (waitpid(job.pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (continuous)
        return stopping ? 0 : 1;
    return result;
}

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
    if ((argc != 2 && argc != 3) || access(argv[1], X_OK) < 0) {
        fprintf(stderr, "Usage: alice-kettle [--continuous] WORKER [PHRASE_FILE]\n");
        return 2;
    }
    struct aliases configured = {0};
    const struct aliases *aliases = &defaults;
    if (argc == 3) {
        if (!load_aliases(argv[2], &configured))
            return 2;
        aliases = &configured;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in endpoint = {
        .sin_family = AF_INET, .sin_port = htons(9875), .sin_addr.s_addr = htonl(0x7f000001u)
    };
    if (fd < 0 || connect(fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) < 0) {
        perror("Quasar connection");
        return 1;
    }
    fprintf(stderr, "Listening for kettle voice commands\n");
    int result = listen_for_commands(fd, argv[1], aliases, continuous);
    close(fd);
    return result;
}
