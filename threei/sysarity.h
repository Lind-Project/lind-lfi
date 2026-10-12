// Number of arguments of each x86-64 Linux system call.
//
// A program makes a system call with whatever the unused argument registers
// happen to hold; the library behind Lind's calls always passes six arguments,
// the unused ones zero. Handlers written for Lind (grates) rely on that: for
// example imfs-grate reads a fourth argument of fchmodat as its flags, though
// the kernel's fchmodat has three. So the arguments beyond a call's arity are
// cleared before a handler sees them.
#pragma once
#include <stdint.h>

#define SYS_ARITY_MAX 335

static const uint8_t sys_arity[SYS_ARITY_MAX] = {
    3, 3, 3, 1, 2, 2, 2, 3, 3, 6, 3, 2, 1, 4, 4, 0, 3, 4, 4, 3,  /* 0..19 */
    3, 2, 1, 5, 0, 5, 3, 3, 3, 3, 3, 3, 1, 2, 0, 2, 2, 1, 3, 0,  /* 20..39 */
    4, 3, 3, 3, 6, 6, 3, 3, 2, 3, 2, 3, 3, 4, 5, 5, 5, 0, 0, 3,  /* 40..59 */
    1, 4, 2, 1, 3, 3, 4, 1, 2, 4, 5, 3, 3, 2, 1, 1, 2, 2, 3, 2,  /* 60..79 */
    1, 1, 2, 2, 1, 2, 2, 1, 2, 3, 2, 2, 3, 3, 3, 1, 2, 2, 2, 1,  /* 80..99 */
    1, 4, 0, 3, 0, 1, 1, 0, 0, 2, 0, 0, 0, 2, 2, 2, 2, 3, 3, 3,  /* 100..119 */
    3, 1, 1, 1, 1, 2, 2, 2, 4, 3, 2, 2, 2, 3, 1, 1, 2, 2, 2, 3,  /* 120..139 */
    2, 3, 2, 2, 3, 1, 1, 1, 2, 2, 2, 1, 0, 0, 3, 2, 1, 5, 2, 1,  /* 140..159 */
    2, 1, 0, 1, 2, 5, 2, 2, 1, 4, 2, 2, 1, 3, 2, 3, 2, 1, 5, 4,  /* 160..179 */
    3, 5, 5, 5, 3, 3, 0, 3, 5, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2,  /* 180..199 */
    2, 1, 6, 3, 3, 1, 2, 1, 5, 3, 3, 1, 3, 1, 4, 4, 5, 3, 1, 0,  /* 200..219 */
    4, 4, 3, 4, 2, 1, 1, 2, 2, 2, 4, 1, 4, 4, 3, 2, 0, 6, 3, 5,  /* 220..239 */
    4, 1, 5, 5, 2, 3, 4, 5, 5, 4, 5, 3, 2, 0, 3, 2, 4, 4, 3, 4,  /* 240..259 */
    5, 3, 4, 3, 4, 5, 3, 4, 3, 3, 6, 5, 1, 2, 3, 6, 4, 4, 4, 6,  /* 260..279 */
    4, 6, 3, 2, 1, 4, 4, 2, 4, 4, 2, 1, 3, 2, 1, 5, 5, 4, 5, 5,  /* 280..299 */
    2, 5, 4, 5, 3, 2, 1, 4, 2, 3, 6, 6, 5, 3, 3, 4, 5, 3, 3, 2,  /* 300..319 */
    5, 3, 5, 1, 3, 3, 6, 6, 6, 4, 2, 1, 5, 6, 4,
};

static inline void
sys_clear_unused_args(long nr, uint64_t a[6])
{
    if ((unsigned long) nr < SYS_ARITY_MAX) {
        for (int i = sys_arity[nr]; i < 6; i++)
            a[i] = 0;
    }
}
