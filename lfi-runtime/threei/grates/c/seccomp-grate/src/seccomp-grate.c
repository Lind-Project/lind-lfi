#include "seccomp.h"
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <seccomp-config.conf> <cage_binary> [args...]\n",
            argv[0]);
    exit(EXIT_FAILURE);
  }

  // parse the config file provided via the first command-line argument
  parse_config(argv[1]);

  // using semaphores for synchronizing the grate and cage.
  //
  // this ensures that all the initalization is done by the grate.
  sem_t *sem = mmap(NULL, sizeof(*sem), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANON, -1, 0);
  sem_init(sem, 1, 0);

  int grateid = getpid();
  pid_t cageid = fork();

  if (cageid < 0) {
    perror("fork failed");
    exit(EXIT_FAILURE);
  } else if (cageid == 0) {
    // wait for grate to register handlers
    sem_wait(sem);

    if (execv(argv[2], &argv[2]) == -1) {
      perror("execv failed");
      exit(EXIT_FAILURE);
    }
  }

  // get the memory address of blacklist_handler
  uint64_t fn_ptr = (uint64_t)(uintptr_t)&blacklist_handler;

  subject_cage = (uint64_t)cageid;

  // Protection must be complete before the cage runs: if any registration
  // fails, kill the cage rather than let it run partly unconfined.
#define MUST(call)                                                             \
  do {                                                                         \
    int r_ = (call);                                                           \
    if (r_ != 0) {                                                             \
      fprintf(stderr, "seccomp-grate: registration failed (%d): %s\n", r_,     \
              #call);                                                          \
      kill(cageid, SIGKILL);                                                   \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

  // loop to register blacklisted syscall handlers
  for (int i = 0; i < MAX_SYSCALLS; i++) {
    if (syscall_mode[i] == BL) {
      MUST(register_handler(cageid, i, grateid, fn_ptr));
    }
  }
  // Linux call numbers beyond the config's range follow its default rule.
  for (int i = MAX_SYSCALLS; i < SEC_LINUX_SLOTS; i++)
    if (default_rule == BL)
      MUST(register_handler(cageid, i, grateid, fn_ptr));
  // The routing operations themselves.
  MUST(register_handler(cageid, SEC_SYS_REGISTER, grateid,
                        (uint64_t)(uintptr_t)&register_guard_handler));
  MUST(register_handler(cageid, SEC_SYS_COPY_HTAB, grateid,
                        (uint64_t)(uintptr_t)&copy_htab_guard_handler));

  // resume execution of the cage
  sem_post(sem);

  int status;

  // The grate's exit status is the cage's: a failed or signaled cage must fail the caller.
  if (wait(&status) < 0)
    return EXIT_FAILURE;

  sem_destroy(sem);
  munmap(sem, sizeof(*sem));

  return (WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
}
