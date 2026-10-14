#ifndef SECCOMP_H
#define SECCOMP_H

#include <errno.h>
#include <lind_syscall.h>
#include <stdint.h>

#define MAX_SYSCALLS 335  /* lfi: was 334; covers numbers up to 334 */

typedef enum {
  MODE_UNASSIGNED = -1, // Catches entries before a section header
  WL = 0,               // Whitelist
  BL = 1,               // Blacklist
  MODE_DEFAULT = 2      // Internal parser state for the [default] section
} seccomp_mode_t;

// function ptr for storing syscall handlers
typedef int (*syscall_handler_t)(uint64_t cageid, uint64_t arg1,
                                 uint64_t arg1cage, uint64_t arg2,
                                 uint64_t arg2cage, uint64_t arg3,
                                 uint64_t arg3cage, uint64_t arg4,
                                 uint64_t arg4cage, uint64_t arg5,
                                 uint64_t arg5cage, uint64_t arg6,
                                 uint64_t arg6cage);

// lookup mapping structure
typedef struct {
  const char *name;
  int num;
} syscall_entry_t;

// The cage this grate confines (set before the cage runs).
extern uint64_t subject_cage;
extern seccomp_mode_t default_rule;

// Threei's own routing operations. They live in the subject's handler table
// like any system call, so the subject could use them to remove the denials
// above. The grate therefore mediates them too (see seccomp.c):
//  * register_handler may not touch the subject's own table, may not empty a
//    slot that carries a denial or a guard;
//  * copy_handler_table_to_cage may only copy the subject's own table (which
//    keeps its denials) and may not overwrite the subject's table.
// Everything else the subject may do to its descendants' tables stays allowed.
#define SEC_SYS_REGISTER 1001
#define SEC_SYS_COPY_HTAB 1003
#define SEC_ALT_BASE 1008 /* first alternate (routing) call number */
#define SEC_DEREGISTER 500 /* grate id that restores the default handler */
#define SEC_LINUX_SLOTS 512 /* the runtime's handler table covers 0-511 */

// array to track list type
extern seccomp_mode_t syscall_mode[MAX_SYSCALLS];

// handler for blacklisted syscalls
int blacklist_handler(uint64_t cageid, uint64_t arg1, uint64_t arg1cage,
                      uint64_t arg2, uint64_t arg2cage, uint64_t arg3,
                      uint64_t arg3cage, uint64_t arg4, uint64_t arg4cage,
                      uint64_t arg5, uint64_t arg5cage, uint64_t arg6,
                      uint64_t arg6cage);

#define SEC_GUARD_ARGS                                                         \
  uint64_t cageid, uint64_t arg1, uint64_t arg1cage, uint64_t arg2,            \
      uint64_t arg2cage, uint64_t arg3, uint64_t arg3cage, uint64_t arg4,      \
      uint64_t arg4cage, uint64_t arg5, uint64_t arg5cage, uint64_t arg6,      \
      uint64_t arg6cage
int register_guard_handler(SEC_GUARD_ARGS);
int copy_htab_guard_handler(SEC_GUARD_ARGS);
// True if the subject may not install or remove a handler for this call number.
int slot_is_protected(uint64_t callnum);

// helper function to clean whitespaces
char *trim_whitespace(char *str);

// function to parse INI config file
void parse_config(const char *filename);

#endif
