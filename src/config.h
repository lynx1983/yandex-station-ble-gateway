#ifndef CONFIG_H
#define CONFIG_H

/* Defaults shared by the voice client, the tests, and the documentation. */
#define DEFAULT_JOB_TIMEOUT_MS 60000
#define DEFAULT_PAIR_TIMEOUT_MS 75000
#define DEFAULT_STOP_GRACE_MS 1000
#define DEFAULT_SESSION_TIMEOUT_MS 90000
#define DEFAULT_SPEECH_TIMEOUT_MS 8000

/* Return a positive millisecond value from the environment or the fallback. */
long long config_ms(const char *name, long long fallback);

#endif
