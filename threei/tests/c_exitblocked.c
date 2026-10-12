// exit() from one thread must end the process even if another thread is blocked
// in a futex (exit_group kills the others); expected exit status 3.
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *blocked(void *a){ pthread_mutex_lock(&m); pthread_mutex_lock(&m); return 0; }   /* blocks forever in futex */
static void *quitter(void *a){ usleep(300000); exit(3); }
int main(void){ pthread_t t,u; pthread_create(&t,0,blocked,0); pthread_create(&u,0,quitter,0); for(;;) sleep(1); }
