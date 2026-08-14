/* Android/Quest AArch64 fiber backend for G-Diffuser's cooperative N64 scheduler.
 *
 * Android's Bionic headers expose ucontext_t, but the runtime does not export the legacy
 * getcontext/makecontext/swapcontext functions.  Use libucontext directly so n64_sched.c can keep
 * the exact same single-thread cooperative scheduling model as the desktop port.
 */
#include "gdx_fiber.h"

/* libucontext's AArch64 ABI header uses the generic struct tag `sigcontext`, the same tag Bionic
 * exposes later through <signal.h>. Rename only libucontext's private tag in this translation unit
 * so both ABI descriptions can coexist without changing either layout. */
#define sigcontext gdx_libucontext_sigcontext
#include <libucontext/libucontext.h>
#undef sigcontext

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MAP_STACK
#define MAP_STACK 0
#endif
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0
#endif
#endif

struct GdxFiber {
    libucontext_ucontext_t ctx;
    void* mapping;
    size_t mappingLen;
    GdxFiberEntry entry;
    void* arg;
    int started;
    int isHost;
};

static __thread GdxFiber* sCurrent = NULL;
static __thread GdxFiber* sTrampolineArg = NULL;

static void gdx_fiber_libucontext_trampoline(void) {
    GdxFiber* f = sTrampolineArg;
    if (f == NULL || f->entry == NULL) {
        abort();
    }
    f->entry(f->arg);
    abort();
}

GdxFiber* gdx_fiber_convert_thread(void) {
    GdxFiber* f = (GdxFiber*)calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->isHost = 1;
    f->started = 1;
    sCurrent = f;
    return f;
}

GdxFiber* gdx_fiber_create(GdxFiberEntry entry, void* arg, size_t stackSize) {
    if (entry == NULL) {
        return NULL;
    }
    if (stackSize == 0) {
        stackSize = 1024u * 1024u;
    }

    const long pageSc = sysconf(_SC_PAGESIZE);
    const size_t page = pageSc > 0 ? (size_t)pageSc : 4096u;
    const size_t usable = (stackSize + page - 1u) & ~(page - 1u);
    const size_t total = usable + page;

    GdxFiber* f = (GdxFiber*)calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }

    void* mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (mem == MAP_FAILED) {
        free(f);
        return NULL;
    }
    (void)mprotect(mem, page, PROT_NONE);

    if (libucontext_getcontext(&f->ctx) != 0) {
        munmap(mem, total);
        free(f);
        return NULL;
    }

    f->ctx.uc_link = NULL;
    f->ctx.uc_stack.ss_sp = (char*)mem + page;
    f->ctx.uc_stack.ss_size = usable;
    f->ctx.uc_stack.ss_flags = 0;
    f->mapping = mem;
    f->mappingLen = total;
    f->entry = entry;
    f->arg = arg;
    return f;
}

void gdx_fiber_switch(GdxFiber* to) {
    GdxFiber* from = sCurrent;
    if (from == NULL || to == NULL) {
        abort();
    }

    sCurrent = to;
    if (!to->started) {
        to->started = 1;
        sTrampolineArg = to;
        libucontext_makecontext(&to->ctx, gdx_fiber_libucontext_trampoline, 0);
    }

    if (libucontext_swapcontext(&from->ctx, &to->ctx) != 0) {
        abort();
    }
}

unsigned long gdx_fiber_current_thread_id(void) {
#ifdef SYS_gettid
    return (unsigned long)syscall(SYS_gettid);
#else
    return (unsigned long)getpid();
#endif
}
