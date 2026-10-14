// MAP_SHARED: shared across fork, process-shared semaphores and mutexes,
// file-backed sharing, and the restrictions.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    // Anonymous shared memory is seen by both sides of a fork.
    volatile int *v = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(v != MAP_FAILED);
    v[0] = 1;
    pid_t pid = fork();
    if (pid == 0) { v[0] += 10; v[1] = 7; _exit(0); }
    waitpid(pid, NULL, 0);
    assert(v[0] == 11 && v[1] == 7);

    // A process-shared semaphore holds a child until the parent is ready.
    sem_t *sem = mmap(NULL, sizeof *sem, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(sem != MAP_FAILED && sem_init(sem, 1, 0) == 0);
    pid = fork();
    if (pid == 0) {
        sem_wait(sem);
        v[2] = 99;
        _exit(0);
    }
    usleep(100000);
    assert(v[2] == 0);              // still blocked
    sem_post(sem);
    waitpid(pid, NULL, 0);
    assert(v[2] == 99);

    // Ping-pong with a timeout: futex errors come back as proper errnos.
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    assert(sem_timedwait(sem, &ts) == -1 && errno == ETIMEDOUT);

    // A process-shared mutex protecting a shared counter, several processes.
    pthread_mutex_t *mu = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_setpshared(&at, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(mu, &at);
    volatile long *ctr = (volatile long *)((char *)mu + 256);
    *ctr = 0;
    for (int i = 0; i < 4; i++) {
        if (fork() == 0) {
            for (int k = 0; k < 5000; k++) { pthread_mutex_lock(mu); (*ctr)++; pthread_mutex_unlock(mu); }
            _exit(0);
        }
    }
    while (wait(NULL) > 0) {}
    assert(*ctr == 20000);

    // A shared file mapping writes through to the file.
    int fd = open("/tmp/lfi3i_shm.bin", O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && ftruncate(fd, 8192) == 0);
    char *f = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(f != MAP_FAILED);
    close(fd);                                         // the mapping outlives the descriptor
    strcpy(f, "via mapping");
    pid = fork();
    if (pid == 0) { strcpy(f + 100, "from child"); _exit(0); }
    waitpid(pid, NULL, 0);
    msync(f, 8192, MS_SYNC);
    char buf[32] = {0};
    int r = open("/tmp/lfi3i_shm.bin", O_RDONLY);
    assert(pread(r, buf, 11, 0) == 11 && !strcmp(buf, "via mapping"));
    assert(pread(r, buf, 11, 100) == 11 && !strcmp(buf, "from child"));
    close(r);
    assert(munmap(f, 8192) == 0);
    unlink("/tmp/lfi3i_shm.bin");

    // Partial unmap and re-map keep working; executable shared memory is refused.
    char *big = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(big != MAP_FAILED);
    assert(munmap(big + 4096, 4096) == 0);
    big[0] = 1; big[2 * 4096] = 2;
    assert(mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_ANON, -1, 0) == MAP_FAILED && errno == EACCES);
    assert(mprotect(big, 4096, PROT_READ | PROT_EXEC) == -1 && errno == EACCES);
    assert(mmap(big, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON | MAP_FIXED, -1, 0) == big);

    // A failed exec (not an LFI program) leaves the old image, shared memory
    // included, intact.
    pid = fork();
    if (pid == 0) {
        int r = execl("/bin/true", "true", (char *)0);
        _exit(r == -1 && errno == ENOEXEC && v[0] == 11 ? 55 : 1);
    }
    int st;
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 55);
    printf("[shm] PASS\n");
    return 0;
}
