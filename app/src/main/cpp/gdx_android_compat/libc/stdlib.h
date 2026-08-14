#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

/*
 * Quest/Android compatibility overlay for the decomp's N64 libc declarations.
 * Android arm64 already supplies div_t/ldiv_t/lldiv_t, ssize_t and wchar_t with
 * the ABI-correct LP64 layouts. Re-declaring the N64 32-bit variants causes hard
 * type conflicts and, more importantly, would truncate host-side sizes/pointers.
 */
#include <stdlib.h>
#include <sys/types.h>
#include <stddef.h>

#endif /* LIBC_STDLIB_H */
