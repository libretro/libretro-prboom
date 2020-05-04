// Copyright (c) 1993-2011 PrBoom developers (see AUTHORS)
// Licence: GPLv2 or later (see COPYING)

// Useful utility functions

#include <retro_endianness.h>

#ifdef __GNUC__
#define ATTR(x) __attribute__(x)
#else
#define ATTR(x)
#endif

#define LONG(x) (retro_le_to_cpu32(x))
#define SHORT(x) (retro_le_to_cpu16(x))

void ATTR((noreturn)) die(const char *error, ...);

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
void *xcalloc(size_t n, size_t size);
char *xstrdup(const char *s);

// slurp an entire file into memory or kill yourself
size_t read_or_die(void **ptr, const char *file);
void search_path(const char *path);
