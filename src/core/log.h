#ifndef CORE_LOG_H
#define CORE_LOG_H

#include <stdio.h>
#include <stdlib.h>

/* Verbosity levels: 0=errors only, 1=+warnings, 2=+trace */
extern int verbose_level;

#define LOG_WARN(...)  do { if (verbose_level >= 1) fprintf(stderr, __VA_ARGS__); } while (0)
#define LOG_TRACE(...) do { if (verbose_level >= 2) fprintf(stderr, __VA_ARGS__); } while (0)

/* Hard failure for unimplemented/unknown code paths -- prints and aborts */
#define FATAL(...) do { fprintf(stderr, "FATAL: " __VA_ARGS__); abort(); } while (0)

#endif /* CORE_LOG_H */
