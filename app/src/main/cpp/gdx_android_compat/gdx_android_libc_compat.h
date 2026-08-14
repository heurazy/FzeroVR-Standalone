#pragma once

/*
 * Force-included before every F-Zero X decomp translation unit on Android arm64.
 *
 * The decomp's quoted includes resolve its N64 libc headers before normal -I
 * search paths, so a regular include-directory overlay cannot intercept them.
 * Load Bionic's ABI-correct definitions first, then mark only the two conflicting
 * N64 compatibility headers as already satisfied.
 */
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>

#ifndef LIBC_STDDEF_H
#define LIBC_STDDEF_H 1
#endif
#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H 1
#endif
