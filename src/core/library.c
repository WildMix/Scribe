/*
 * Shared-library anchor translation unit.
 *
 * The public Scribe API functions are implemented in the core source files and
 * linked into both the CLI and the shared library target. Keeping this file
 * explicit gives CMake a stable source for the shared library without
 * duplicating implementation code here.
 */
#include "scribe/scribe.h"

/* Public symbols are implemented by the core translation units. This file keeps
 * the shared library target explicit without duplicating the static core. */
