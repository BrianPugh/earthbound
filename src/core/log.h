#ifndef CORE_LOG_H
#define CORE_LOG_H

/*
 * Logging facade.
 *
 * Desktop builds route LOG_WARN / LOG_TRACE / FATAL through fprintf(stderr, ...)
 * gated by `verbose_level`. Embedded builds (-DEB_EMBEDDED) make LOG_WARN and
 * LOG_TRACE no-ops (drops a stdio dependency, lets the linker garbage-collect
 * the format strings) and turn FATAL into a CPU trap.
 *
 * Ports that want their own diagnostic sink can override by defining
 * EB_EMBEDDED and providing a wrapper header force-included before this one.
 */

#ifdef EB_EMBEDDED

#define LOG_WARN(...)  ((void)0)
#define LOG_TRACE(...) ((void)0)
#define FATAL(...)     __builtin_trap()

#else /* !EB_EMBEDDED */

#include <stdio.h>
#include <stdlib.h>

/* Verbosity levels: 0=errors only, 1=+warnings, 2=+trace */
extern int verbose_level;

#define LOG_WARN(...)  do { if (verbose_level >= 1) fprintf(stderr, __VA_ARGS__); } while (0)
#define LOG_TRACE(...) do { if (verbose_level >= 2) fprintf(stderr, __VA_ARGS__); } while (0)

/* Hard failure for unimplemented/unknown code paths -- prints and aborts */
#define FATAL(...) do { fprintf(stderr, "FATAL: " __VA_ARGS__); abort(); } while (0)

#endif /* EB_EMBEDDED */

#endif /* CORE_LOG_H */
