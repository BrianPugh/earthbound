#ifndef CORE_STATE_DUMP_H
#define CORE_STATE_DUMP_H

#include <stdbool.h>

/* Dump all module state to a binary file.
 * Format: "EBSD" header + tagged sections + 0xFFFF terminator.
 * Returns true on success. */
bool state_dump_save(const char *path);

#endif /* CORE_STATE_DUMP_H */
