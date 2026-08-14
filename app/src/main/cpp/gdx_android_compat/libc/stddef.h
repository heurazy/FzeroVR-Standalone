#ifndef LIBC_STDDEF_H
#define LIBC_STDDEF_H

/*
 * Quest/Android compatibility overlay for the N64 decomp libc header.
 *
 * The original N64 header hard-codes ptrdiff_t to s32. That is correct for the
 * 32-bit N64 ABI but conflicts with Android's LP64 ABI, where ptrdiff_t is long.
 * Host pointers must keep the platform type or pointer subtraction can truncate.
 */
#include <stddef.h>

#endif /* LIBC_STDDEF_H */
