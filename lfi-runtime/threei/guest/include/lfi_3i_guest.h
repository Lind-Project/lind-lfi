// Guest-side API of the LFI 3i runtime, for programs running as cages.
//
// A grate is a program that interposes on other cages' system calls:
//
//   1. It defines handler functions with the signature
//        int handler(uint64_t cageid,
//                    uint64_t arg1, uint64_t arg1cage, ... uint64_t arg6, uint64_t arg6cage);
//      where cageid is the grate's own id and every argument comes with the id
//      of the cage whose memory (or file descriptor table) it refers to.
//   2. It forks; the child registers the handlers for itself with
//      register_handler() and execs the cage to interpose on.
//   3. A handler either answers the call itself, or forwards it (possibly with
//      modified arguments) with make_threei_call(), acting on behalf of the
//      cage the call came from. copy_data_between_cages() moves data between
//      the grate's memory and the cage's.
//
// The API mirrors Lind's lind_syscall.h.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lfi_3i_abi.h"

#define GRATE_MEMORY_FLAG LFI3I_GRATE_MEMORY_FLAG
#define THREEI_DEREGISTER LFI3I_DEREGISTER
#define ELINDAPIABORTED   LFI3I_ELINDAPIABORTED
#define ELINDESRCH        LFI3I_ELINDESRCH

// Placeholders for unused call slots (as in Lind).
#define UNUSED_ARG  0xDEADBEEFDEADBEEFull
#define UNUSED_ID   0xCAFEBABECAFEBABEull
#define UNUSED_NAME 0xFEEDFACEFEEDFACEull

// Invokes system call 'callnumber' as cage 'self' on behalf of cage 'target'.
// 'self' must be the calling cage's own id (getpid()). The call goes through
// self's handler table, so it reaches the next grate in the stack or the
// default implementation. If translate_errno is non-zero, a negative errno
// result is turned into -1 with errno set; otherwise the raw result is
// returned.
int make_threei_call(unsigned int callnumber, uint64_t callname,
    uint64_t self_cageid, uint64_t target_cageid,
    uint64_t arg1, uint64_t arg1cageid,
    uint64_t arg2, uint64_t arg2cageid,
    uint64_t arg3, uint64_t arg3cageid,
    uint64_t arg4, uint64_t arg4cageid,
    uint64_t arg5, uint64_t arg5cageid,
    uint64_t arg6, uint64_t arg6cageid,
    int translate_errno);

// Makes cage 'targetcage' send its system call 'targetcallnum' to the handler
// at 'in_grate_fn_ptr_u64' in cage 'this_grate_id'. Pass THREEI_DEREGISTER as
// the grate id to restore the default. Returns 0 on success.
int register_handler(uint64_t targetcage, uint64_t targetcallnum,
    uint64_t this_grate_id, uint64_t in_grate_fn_ptr_u64);

// Copies len bytes (or, with copytype LFI3I_COPY_STRNCPY, a NUL-terminated
// string of at most len bytes) from srcaddr in cage srccage to destaddr in
// cage destcage. Returns destaddr on success, or ELINDAPIABORTED (for every kind of failure, as in Lind).
int copy_data_between_cages(uint64_t thiscage, uint64_t targetcage,
    uint64_t srcaddr, uint64_t srccage, uint64_t destaddr, uint64_t destcage,
    uint64_t len, uint64_t copytype);

// Full-width variants used by the Rust library (see lfi3i.c).
long lfi3i_copy_data_long(uint64_t srcaddr, uint64_t srccage, uint64_t destaddr,
    uint64_t destcage, uint64_t len, uint64_t copytype);
long lfi3i_copy_htab_long(uint64_t srccage, uint64_t targetcage);

// Replaces targetcage's handler table with a copy of srccage's.
int copy_handler_table_to_cage(uint64_t srccage, uint64_t targetcage);

#ifdef __cplusplus
}
#endif
