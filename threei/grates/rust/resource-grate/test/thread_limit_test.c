// F04 regression: the resource grate must recognise pthread_create as a thread (CLONE_THREAD in the
// native clone flags) and enforce `resource events N`. Threads are created until creation fails;
// the limit has to bite, accounting has to be released when threads end, and fork must not be
// counted as a thread.
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static void *blocked(void *a) { (void) a; pthread_mutex_lock(&gate); pthread_mutex_unlock(&gate); return 0; }

#define MAXT 32
#define LIMIT 4   /* `resource events 4` in test_threads.cfg */
static int last_err;
static int make_threads(pthread_t *t)
{
    int n = 0;
    pthread_mutex_lock(&gate);
    last_err = 0;
    while (n < MAXT && (last_err = pthread_create(&t[n], 0, blocked, 0)) == 0) n++;
    return n;
}
static void join_threads(pthread_t *t, int n)
{
    pthread_mutex_unlock(&gate);
    for (int i = 0; i < n; i++) pthread_join(t[i], 0);
}

int main(void)
{
    pthread_t t[MAXT];
    int fails = 0;
    int n1 = make_threads(t);
    printf("created %d threads before the limit\n", n1);
    if (n1 != LIMIT) { printf("FAIL: expected exactly %d threads, created %d\n", LIMIT, n1); fails++; }
    if (last_err != EAGAIN) { printf("FAIL: the refused pthread_create returned %d, not EAGAIN\n", last_err); fails++; }
    join_threads(t, n1);
    usleep(100000);
    // accounting released: the same number can be created again
    int n2 = make_threads(t);
    printf("created %d threads after joining\n", n2);
    if (n2 != LIMIT || last_err != EAGAIN) { printf("FAIL: accounting not released (%d then %d, err %d)\n", n1, n2, last_err); fails++; }
    join_threads(t, n2);
    // processes are not threads: more forks than the thread limit must work
    for (int i = 0; i < n1 + 3; i++) {
        pid_t p = fork();
        if (p < 0) { printf("FAIL: fork %d failed: %d\n", i, errno); fails++; break; }
        if (p == 0) _exit(0);
        int st;
        if (waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("FAIL: forked child %d did not exit cleanly (status %d)\n", i, st); fails++; break;
        }
    }
    printf(fails ? "thread_limit: FAILED\n" : "thread_limit: PASS\n");
    return fails ? 1 : 0;
}
