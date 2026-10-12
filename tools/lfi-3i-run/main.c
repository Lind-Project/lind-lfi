// lfi-3i-run: runs a program as cage 1 of an LFI 3i system.
//
// Cages can fork/exec/wait, have private file descriptor tables, and can
// interpose on each other's system calls through grates. The program given
// here is typically a grate (or a plain program); a grate launches the cages
// it interposes on by forking, registering handlers for the child and execing
// the cage (see the examples).
#include "lfi_3i.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *
xrealloc(void *p, size_t size)
{
    void *q = realloc(p, size);
    if (!q) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return q;
}

static void
usage(const char *prog)
{
    printf("Usage: %s [OPTION...] PROGRAM [ARG...]\n\n", prog);
    printf("  -h, --help             show help\n");
    printf("  -V, --verbose          verbose output\n");
    printf("  --no-verify            disable code verification (UNSAFE)\n");
    printf("  --no-aslr              disable address space randomization\n");
    printf("  --max-cages=<n>        maximum number of live cages (default 256)\n");
    printf("  --no-net               deny network (non UNIX-domain) sockets\n");
    printf("  --permissive           disable 3i authorization checks (UNSAFE)\n");
    printf("  --env=<var=val>        set an environment variable\n");
    printf("  --dir=<box=host>       map a sandbox directory to a host directory\n");
    printf("  --wd=<dir>             initial working directory in the sandbox\n");
    printf("  --argv0=<name>         argv[0] for the program (default: its path)\n");
    printf("  -r, --restricted       apply --dir/--wd (default maps / to / and uses $PWD)\n");
}

int
main(int argc, char **argv)
{
    bool help = false, verbose = false, verify = true, no_aslr = false;
    bool permissive = false, restricted = false, net = true;
    size_t max_cages = 0;
    const char **envs = NULL;
    int envs_count = 0;
    const char **dirs = NULL;
    int dirs_count = 0;
    const char *wd = NULL;
    const char *argv0 = NULL;

    static struct option long_options[] = {
        { "help", no_argument, 0, 'h' },
        { "verbose", no_argument, 0, 'V' },
        { "no-verify", no_argument, 0, 1 },
        { "no-aslr", no_argument, 0, 2 },
        { "max-cages", required_argument, 0, 3 },
        { "env", required_argument, 0, 4 },
        { "dir", required_argument, 0, 5 },
        { "wd", required_argument, 0, 6 },
        { "permissive", no_argument, 0, 7 },
        { "no-net", no_argument, 0, 8 },
        { "argv0", required_argument, 0, 9 },
        { "restricted", no_argument, 0, 'r' },
        { 0, 0, 0, 0 },
    };
    int opt, idx = 0;
    // '+' stops option parsing at the program name, so the program's own
    // arguments are left alone.
    while ((opt = getopt_long(argc, argv, "+hVr", long_options, &idx)) != -1) {
        switch (opt) {
        case 'h': help = true; break;
        case 'V': verbose = true; break;
        case 1: verify = false; break;
        case 2: no_aslr = true; break;
        case 3: max_cages = strtoul(optarg, NULL, 10); break;
        case 4:
            envs = xrealloc(envs, sizeof(char *) * (++envs_count));
            envs[envs_count - 1] = optarg;
            break;
        case 5:
            dirs = xrealloc(dirs, sizeof(char *) * (++dirs_count));
            dirs[dirs_count - 1] = optarg;
            break;
        case 6: wd = optarg; break;
        case 7: permissive = true; break;
        case 8: net = false; break;
        case 9: argv0 = optarg; break;
        case 'r': restricted = true; break;
        default: return 1;
        }
    }
    const char *progpath = optind < argc ? argv[optind] : NULL;
    if (help || optind >= argc) {
        usage(argv[0]);
        return help ? 0 : 1;
    }

    char cwd[FILENAME_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        return 1;
    }
    const char *all[] = { "/=/", NULL };
    dirs = xrealloc(dirs, sizeof(char *) * (++dirs_count));
    dirs[dirs_count - 1] = NULL;
    envs = xrealloc(envs, sizeof(char *) * (++envs_count));
    envs[envs_count - 1] = NULL;

    struct Lfi3iEngine *e = lfi3i_new((struct Lfi3iOptions) {
        .max_cages = max_cages,
        .verify = verify,
        .verbose = verbose,
        .no_aslr = no_aslr,
        .permissive = permissive,
        .net = net,
        .dir_maps = restricted ? dirs : all,
        .wd = restricted ? wd : cwd,
    });
    if (!e) {
        fprintf(stderr, "failed to create the 3i engine\n");
        return 1;
    }
    if (argv0)
        argv[optind] = (char *) argv0;   // (the program is loaded from the path in progpath)
    int status = lfi3i_run(e, progpath, argc - optind,
        (const char **) &argv[optind], envs);
    lfi3i_free(e);
    // Report the init cage's status the way a shell would.
    if (status & 0x7f)
        return 128 + (status & 0x7f);
    return (status >> 8) & 0xff;
}
