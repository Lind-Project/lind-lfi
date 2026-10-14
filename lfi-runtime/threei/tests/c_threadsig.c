// Signals sent from one thread to another (pthread_kill). Delivering to a different worker thread
// used to deadlock the runtime on the process's thread-list lock.
//   c_threadsig handler   handler runs, in the target thread
//   c_threadsig ignored   an ignored signal is discarded, sender continues
//   c_threadsig fatal     default-fatal signal to a worker terminates the whole cage
//   c_threadsig race      repeated signals racing the target's exit
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t hits;
static volatile pthread_t handler_thread;
static volatile int stop;
static pthread_t worker;

static void on_usr1(int s) { (void) s; handler_thread = pthread_self(); hits++; }
static void *spin(void *a) { (void) a; while (!stop) usleep(1000); return 0; }
static void *quick(void *a) { (void) a; return 0; }

int main(int argc, char **argv)
{
    alarm(20);   // a deadlock shows up as SIGALRM, not as a hung test
    const char *m = argc > 1 ? argv[1] : "handler";
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    if (!strcmp(m, "handler") || !strcmp(m, "race")) sa.sa_handler = on_usr1;
    else if (!strcmp(m, "ignored")) sa.sa_handler = SIG_IGN;
    else sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, 0);

    if (!strcmp(m, "race")) {
        for (int i = 0; i < 300; i++) {
            pthread_t t;
            if (pthread_create(&t, 0, quick, 0)) { puts("FAIL create"); return 1; }
            pthread_kill(t, SIGUSR1);   // may already be gone: any result but a hang or crash is fine
            pthread_join(t, 0);
        }
        puts("threadsig race: PASS");
        return 0;
    }

    if (pthread_create(&worker, 0, spin, 0)) { puts("FAIL create"); return 1; }
    usleep(20000);
    int r = pthread_kill(worker, SIGUSR1);
    if (r) { printf("FAIL pthread_kill: %d\n", r); return 1; }
    if (!strcmp(m, "fatal")) {
        for (int i = 0; i < 2000; i++) usleep(1000);   // the cage must be terminated before this ends
        puts("FAIL: survived a fatal signal");
        return 1;
    }
    for (int i = 0; i < 2000 && !strcmp(m, "handler") && !hits; i++) usleep(1000);
    stop = 1;
    pthread_join(worker, 0);
    if (!strcmp(m, "handler")) {
        if (hits != 1) { printf("FAIL: handler ran %d times\n", (int) hits); return 1; }
        if (!pthread_equal(handler_thread, worker)) { puts("FAIL: handler ran in the wrong thread"); return 1; }
    } else if (hits) { puts("FAIL: ignored signal ran a handler"); return 1; }
    printf("threadsig %s: PASS\n", m);
    return 0;
}
