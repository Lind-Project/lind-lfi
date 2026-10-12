// ABI shared by the LFI 3i host runtime and the guest-side shim library.
//
// 3i ("three-eye", the inter-cage interposition interface) lets a cage
// intercept the system calls of other cages. A cage that interposes on others
// is a "grate". The model mirrors Lind's 3i:
//
//  * Every cage owns a handler table that maps a system call number to either
//    the default implementation or (grate, function) in a grate cage.
//  * register_handler / copy_handler_table_to_cage / copy_data_between_cages
//    are themselves routed through the handler table (they are system calls
//    numbered 1001-1003), so grates can interpose on them too.
//  * Every system call argument travels together with the id of the cage whose
//    memory/descriptor table the argument refers to.
#pragma once

#include <stdint.h>

// System calls understood by the 3i layer, in addition to the Linux ones.
#define LFI3I_SYS_CALL           1000 // make_threei_call(struct lfi3i_call *)
#define LFI3I_SYS_REGISTER       1001 // register_handler
#define LFI3I_SYS_COPY_DATA      1002 // copy_data_between_cages
#define LFI3I_SYS_COPY_HTAB      1003 // copy_handler_table_to_cage
#define LFI3I_SYS_SETENTRY       1010 // guest announces its grate entry points
#define LFI3I_SYS_CAPTURE        1011 // next clone creates a grate context
#define LFI3I_SYS_COLLECT        1012 // take the context created by the clone
#define LFI3I_SYS_SELF           1013 // the calling cage's id (never interposable)

// Pass as handlefunccage to register_handler to restore the default handler.
#define LFI3I_DEREGISTER         500
// Error codes (same values as Lind's threei_const).
#define LFI3I_ELINDAPIABORTED    0xE0010001u
#define LFI3I_ELINDESRCH         0xE0010002u
// Flag in an argument's cage id: the pointer refers to the grate's own memory.
// (Accepted and ignored: the cage id alone identifies the address space.)
#define LFI3I_GRATE_MEMORY_FLAG  ((uint64_t) 1 << 63)
#define LFI3I_CAGEID_MASK        (~LFI3I_GRATE_MEMORY_FLAG)

// copy_data_between_cages copy types.
#define LFI3I_COPY_MEMCPY        0
#define LFI3I_COPY_STRNCPY       1

// Payload of the make_threei_call system call (passed by pointer).
struct lfi3i_call {
    uint64_t callnum;
    uint64_t callname;
    uint64_t self;
    uint64_t target;
    uint64_t arg[6];
    uint64_t argcage[6];
};

// Request written by the runtime into the grate's memory to invoke a handler.
struct lfi3i_req {
    uint64_t fn;     // handler address in the grate
    uint64_t cageid; // the grate's own cage id
    uint64_t arg[6];
    uint64_t argcage[6];
};
