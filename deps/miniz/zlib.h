/* zlib.h shim: route the zlib API to the vendored miniz, which provides
 * zlib-compatible names (deps/miniz is on the global include path). */
#ifndef ZLIB_SHIM_H
#define ZLIB_SHIM_H
#include "miniz.h"
#endif
