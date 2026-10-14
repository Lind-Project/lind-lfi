// The grate's geteuid handler crashes. The cage must survive: the failing
// call returns an error, and everything else keeps working.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    unsigned e1 = geteuid();
    printf("[cage] first geteuid -> %d\n", (int)e1);
    assert((int)e1 == -3);          // -ESRCH: the handler's grate died
    unsigned e2 = geteuid();
    assert((int)e2 == -3);          // the grate is gone: still fail-closed
    assert(getuid() == 1000);       // calls nobody interposed on still work
    // Leave a marker: depending on timing the grate may reap this cage before
    // it dies, so the exit status is not always visible to the test driver.
    close(open("/tmp/lfi3i_crash_ok", O_CREAT | O_WRONLY, 0600));
    printf("[cage] OK\n");
    return 0;
}
