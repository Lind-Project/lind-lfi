// An in-memory file system grate. Paths under /mem/ live in the grate's own
// memory; everything else is forwarded to the default implementation. The
// grate hands out its own file descriptor numbers (above the real range) for
// its files and intercepts the operations on them, moving data between its
// memory and the cage's buffers with copy_data_between_cages().
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lind_syscall.h"

#define VFD_BASE 2000
#define MAXFILES 16
#define MAXOPEN 32

struct file { char name[64]; char *data; size_t len, cap; };
struct open { int used; struct file *f; size_t off; };

static struct file files[MAXFILES];
static struct open opens[MAXOPEN];
static int me;

#define ARGS uint64_t cageid, uint64_t a1, uint64_t c1, uint64_t a2, uint64_t c2, \
             uint64_t a3, uint64_t c3, uint64_t a4, uint64_t c4, uint64_t a5,   \
             uint64_t c5, uint64_t a6, uint64_t c6
#define FORWARD(nr) make_threei_call(nr, 0, me, c1, a1, c1, a2, c2, a3, c3, a4, c4, a5, c5, a6, c6, 0)

static struct open *vfd(uint64_t fd) {
    if (fd < VFD_BASE || fd >= VFD_BASE + MAXOPEN) return NULL;
    struct open *o = &opens[fd - VFD_BASE];
    return o->used ? o : NULL;
}

static int do_open(uint64_t pathaddr, uint64_t pathcage, uint64_t flags, int nr, ARGS) {
    char path[256];
    if (copy_data_between_cages(me, pathcage, pathaddr, pathcage, (uint64_t)path, me,
                                sizeof path, LFI3I_COPY_STRNCPY) == (int)LFI3I_ELINDAPIABORTED)
        return -EFAULT;
    if (strncmp(path, "/mem/", 5) != 0)
        return FORWARD(nr);
    struct file *f = NULL;
    for (int i = 0; i < MAXFILES; i++)
        if (files[i].name[0] && !strcmp(files[i].name, path)) f = &files[i];
    if (!f) {
        if (!(flags & O_CREAT)) return -ENOENT;
        for (int i = 0; i < MAXFILES && !f; i++)
            if (!files[i].name[0]) { f = &files[i]; strncpy(f->name, path, 63); }
        if (!f) return -ENOSPC;
    }
    if (flags & O_TRUNC) f->len = 0;
    for (int i = 0; i < MAXOPEN; i++)
        if (!opens[i].used) {
            opens[i] = (struct open){1, f, (flags & O_APPEND) ? f->len : 0};
            return VFD_BASE + i;
        }
    return -EMFILE;
}

static int h_open(ARGS)   { return do_open(a1, c1, a2, 2, cageid, a1, c1, a2, c2, a3, c3, a4, c4, a5, c5, a6, c6); }
static int h_openat(ARGS) { return do_open(a2, c2, a3, 257, cageid, a1, c1, a2, c2, a3, c3, a4, c4, a5, c5, a6, c6); }

static int h_write(ARGS) {
    struct open *o = vfd(a1);
    if (!o) return FORWARD(1);
    size_t n = a3;
    if (o->off + n > o->f->cap) {
        o->f->cap = (o->off + n) * 2;
        o->f->data = realloc(o->f->data, o->f->cap);
    }
    // cage -> grate
    if (copy_data_between_cages(me, c2, a2, c2, (uint64_t)(o->f->data + o->off), me, n,
                                LFI3I_COPY_MEMCPY) == (int)LFI3I_ELINDAPIABORTED)
        return -EFAULT;
    o->off += n;
    if (o->off > o->f->len) o->f->len = o->off;
    return n;
}

static int h_read(ARGS) {
    struct open *o = vfd(a1);
    if (!o) return FORWARD(0);
    size_t n = a3;
    if (o->off >= o->f->len) return 0;
    if (n > o->f->len - o->off) n = o->f->len - o->off;
    // grate -> cage
    if (copy_data_between_cages(me, c2, (uint64_t)(o->f->data + o->off), me, a2, c2, n,
                                LFI3I_COPY_MEMCPY) == (int)LFI3I_ELINDAPIABORTED)
        return -EFAULT;
    o->off += n;
    return n;
}

static int h_lseek(ARGS) {
    struct open *o = vfd(a1);
    if (!o) return FORWARD(8);
    long off = a2;
    switch ((int)a3) {
    case SEEK_SET: break;
    case SEEK_CUR: off += o->off; break;
    case SEEK_END: off += o->f->len; break;
    default: return -EINVAL;
    }
    if (off < 0) return -EINVAL;
    o->off = off;
    return off;
}

static int h_close(ARGS) {
    struct open *o = vfd(a1);
    if (!o) return FORWARD(3);
    o->used = 0;
    return 0;
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    me = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int c = getpid();
        struct { int nr; void *fn; } t[] = {
            {2, h_open}, {257, h_openat}, {1, h_write}, {0, h_read}, {8, h_lseek}, {3, h_close},
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
            assert(register_handler(c, t[i].nr, me, (uint64_t)(uintptr_t)t[i].fn) == 0);
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    int st;
    wait(&st);
    printf("[imfs] cage exited %d; files in memory: %s (%zu bytes)\n", st, files[0].name, files[0].len);
    assert(st == 0);
    printf("[imfs] PASS\n");
    return 0;
}
