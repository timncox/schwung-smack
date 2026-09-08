/*
 * Compiles the vendored engine with its two allocation calls redirected to
 * the SDRAM bump allocator.
 *
 * Doing it here rather than as a global -D keeps the redirect scoped to this
 * translation unit -- libDaisy and everything else keep the real calloc/free.
 * stdlib.h is included FIRST so the macros rewrite call sites only, never the
 * library's own declarations.
 *
 * Build this file; do NOT add vendor/smack_core.c to the source list too, or
 * you get duplicate symbols.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "patch_alloc.h"

#define calloc patch_calloc
#define free   patch_free

#include "vendor/smack_core.c"
