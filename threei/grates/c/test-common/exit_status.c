// Test cage for the C grates' launchers: exits with a chosen status (or dies of a signal) so a
// test can check that the grate reports the cage's failure instead of exiting 0.
//   exit_status <code>     exit(code)
//   exit_status abort      raise SIGABRT
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    if (!strcmp(argv[1], "abort"))
        raise(SIGABRT);
    return atoi(argv[1]);
}
