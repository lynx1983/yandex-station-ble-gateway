#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "phrases.h"

const struct aliases default_aliases = {
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

const char *const operation_names[] = {
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

enum operation kettle_phrase(const struct aliases *aliases, struct bytes text, int *target)
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

int load_aliases(const char *path, struct aliases *aliases)
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
