// Host-side API of the LFI 3i runtime: runs programs as cages with Lind-style
// inter-cage interposition (grates), process semantics (fork/exec/wait) and
// per-cage file descriptor tables.
#pragma once

#include "lfi_3i_abi.h"
#include "lfi_linux.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Lfi3iEngine;

struct Lfi3iOptions {
    // Maximum number of cages alive at once (each takes a 4GiB sandbox slot of
    // virtual address space).
    size_t max_cages;
    // Stack size for each cage's main thread.
    size_t stacksize;
    // Verify executable code (strongly recommended; disabling is unsafe).
    bool verify;
    bool verbose;
    bool no_aslr;
    // Allow cages to use the network (sockets other than UNIX-domain ones).
    // A grate can restrict this further by interposing on the socket calls.
    bool net;
    // Skip the authorization checks on 3i calls (which otherwise restrict
    // which cages a cage may interpose on / act for).
    bool permissive;
    // Directory mappings and initial working directory, as for LFILinuxOptions.
    const char **dir_maps;
    const char *wd;
};

struct Lfi3iEngine *
lfi3i_new(struct Lfi3iOptions opts);

// Loads 'path' as cage 1 and runs it. Returns the cage's exit status (as
// wait() would report it) once every cage has terminated.
int
lfi3i_run(struct Lfi3iEngine *e, const char *path, int argc, const char **argv,
    const char **envp);

void
lfi3i_free(struct Lfi3iEngine *e);

#ifdef __cplusplus
}
#endif
