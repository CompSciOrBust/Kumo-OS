/*
 * Compiles the vendored FatFs core as part of this module. The vendor sources
 * are built unmodified; warnings this project treats as errors are relaxed
 * for them here.
 */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#include "../../../../vendor/fatfs/source/ff.c"
