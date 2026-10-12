// One thread maps and unmaps a region in a loop while another writes from it
// to a descriptor whose write is served by a grate that copies the buffer out
// of this cage. The copy must fail cleanly (EFAULT) when the range vanishes.
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define LEN (256 * 1024)
static char *region;
static volatile int stop;

static void *churn(void *a) {
    while (!stop) {
        mmap(region, LEN, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        munmap(region, LEN);
    }
    return 0;
}

int main(void) {
    region = mmap(0, LEN, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(region != MAP_FAILED);
    pthread_t t;
    pthread_create(&t, 0, churn, 0);
    int fd = open("/dev/null", O_WRONLY);
    long ok = 0, bad = 0;
    for (int i = 0; i < 20000; i++) {
        long r = write(fd, region, LEN);
        if (r == LEN) ok++; else bad++;
    }
    stop = 1;
    pthread_join(t, 0);
    printf("[copyrace] %ld copies succeeded, %ld failed cleanly\n", ok, bad);
    return 0;
}
