// A sweep over the less common file and process calls.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define DIR "/tmp/lfi3i_misc"

int main(void) {
    mkdir(DIR, 0700);
    // Leftovers from an earlier failed run.
    unlink(DIR "/a"); unlink(DIR "/b"); unlink(DIR "/c"); unlink(DIR "/hard"); unlink(DIR "/sym");
    int fd = open(DIR "/a", O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, "0123456789", 10) == 10);

    // preadv / pwritev
    char b1[4] = {0}, b2[3] = {0};
    struct iovec iov[2] = { { b1, 3 }, { b2, 2 } };
    assert(preadv(fd, iov, 2, 2) == 5);
    assert(!memcmp(b1, "234", 3) && !memcmp(b2, "56", 2));
    struct iovec wv[2] = { { "AB", 2 }, { "CD", 2 } };
    assert(pwritev(fd, wv, 2, 0) == 4);
    char all[11] = {0};
    assert(pread(fd, all, 10, 0) == 10 && !memcmp(all, "ABCD456789", 10));

    // flock, fallocate, sendfile
    assert(flock(fd, LOCK_EX | LOCK_NB) == 0);
    assert(flock(fd, LOCK_UN) == 0);
    assert(fallocate(fd, 0, 0, 4096) == 0);
    struct stat st; assert(fstat(fd, &st) == 0 && st.st_size == 4096);
    int out = open(DIR "/b", O_RDWR | O_CREAT | O_TRUNC, 0600);
    off_t off = 0;
    assert(sendfile(out, fd, &off, 10) == 10 && off == 10);
    char cp[11] = {0};
    assert(pread(out, cp, 10, 0) == 10 && !memcmp(cp, "ABCD456789", 10));

    // names: link, symlink, chmod, utimensat, rename by directory fd
    assert(link(DIR "/a", DIR "/hard") == 0);
    assert(symlink(DIR "/a", DIR "/sym") == 0);
    char lbuf[128] = {0};
    assert(readlink(DIR "/sym", lbuf, sizeof lbuf - 1) > 0);
    assert(chmod(DIR "/a", 0640) == 0);
    assert(stat(DIR "/hard", &st) == 0 && (st.st_mode & 0777) == 0640 && st.st_nlink == 2);
    struct timespec ts[2] = { { 1000000000, 0 }, { 1000000001, 5 } };
    assert(utimensat(AT_FDCWD, DIR "/a", ts, 0) == 0);
    assert(stat(DIR "/a", &st) == 0 && st.st_mtim.tv_sec == 1000000001);
    struct timeval tv[2] = { { 2000000000, 0 }, { 2000000002, 0 } };
    assert(utimes(DIR "/a", tv) == 0);
    assert(stat(DIR "/a", &st) == 0 && st.st_mtim.tv_sec == 2000000002);
    int dfd = open(DIR, O_RDONLY | O_DIRECTORY);
    assert(dfd >= 0);
    assert(renameat(dfd, "b", dfd, "c") == 0);
    assert(faccessat(dfd, "c", R_OK, 0) == 0 && faccessat(dfd, "b", F_OK, 0) == -1);
    assert(fstatat(dfd, "c", &st, 0) == 0 && st.st_size == 10);
    assert(unlinkat(dfd, "c", 0) == 0);
    assert(fchmodat(dfd, "a", 0600, 0) == 0);
    close(dfd);

    // statfs
    struct statfs sf;
    assert(statfs(DIR, &sf) == 0 && sf.f_bsize > 0);
    assert(fstatfs(fd, &sf) == 0 && sf.f_bsize > 0);

    // process information
    struct rusage ru; assert(getrusage(RUSAGE_SELF, &ru) == 0);
    struct tms tm; assert(times(&tm) != (clock_t)-1);
    struct timespec res; assert(clock_getres(CLOCK_MONOTONIC, &res) == 0 && res.tv_nsec > 0);
    struct rlimit rl; assert(getrlimit(RLIMIT_NOFILE, &rl) == 0);
    assert(getpgid(0) == getpid() && getsid(0) == getpid());
    assert(setsid() == getpid());
    assert(setuid(1000) == 0 && setgid(1000) == 0);
    assert(getgroups(0, NULL) == 0);
    sync();

    // cleanup
    close(fd); close(out);
    unlink(DIR "/a"); unlink(DIR "/hard"); unlink(DIR "/sym");
    assert(rmdir(DIR) == 0);
    printf("[misc] PASS\n");
    return 0;
}
