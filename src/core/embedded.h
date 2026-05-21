#ifndef CORE_EMBEDDED_H
#define CORE_EMBEDDED_H

/*
 * Build-attribute macros for embedded ports that need fine-grained control
 * over where data lands at link time.
 *
 * EB_NORELOC
 *   The Game & Watch port splits the binary across RAM (code + small data)
 *   and OSPI external flash (bulk rodata + 3 MB of assets). At launch a
 *   PatchCodeRodataOffset pass walks RAM looking for 32-bit words in the
 *   linker's virtual rodata region (0xCAFE0000...) and rewrites them to
 *   point at the OSPI cache. The scanner only walks RAM, so any rodata
 *   table that itself contains pointers to other rodata (e.g.
 *   const char *names[] = {"a", "b"}) needs to live in RAM, not flash,
 *   or its pointers never get fixed up and dereference into garbage.
 *
 *   Tag such tables with EB_NORELOC. The linker script's overlay rule
 *   pulls .noreloc into RAM alongside .data/.text:
 *       build/earthbound/ *.o (.data .data* .text .text* .noreloc)
 *
 *   Define EB_NORELOC_REQUIRED on the G&W build to activate the
 *   attribute; expands to nothing elsewhere (desktop / Pico / SNES /
 *   any port that does not run PatchCodeRodataOffset).
 */
#ifdef EB_NORELOC_REQUIRED
#define EB_NORELOC __attribute__((section(".noreloc")))
#else
#define EB_NORELOC
#endif

#endif /* CORE_EMBEDDED_H */
