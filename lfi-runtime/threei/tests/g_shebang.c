// exec of a #! script whose interpreter is an LFI program.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
    assert(argc >= 2);                       // argv[1]: path of the interpreter (c_hello)
    char script[] = "/tmp/lfi3i_shebang_script";
    char out[] = "/tmp/lfi3i_shebang_out";
    int f = open(script, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    assert(f >= 0);
    char line[512];
    int n = snprintf(line, sizeof line, "#!%s -x\nignored\n", argv[1]);
    write(f, line, n);
    close(f);
    pid_t pid = fork();
    if (pid == 0) {
        int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(o, 1);
        char *av[] = { script, "arg1", NULL };
        execv(script, av);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    char buf[256] = {0};
    int r = open(out, O_RDONLY);
    read(r, buf, sizeof buf - 1);
    printf("[shebang] interpreter output: %s", buf);
    // c_hello prints argc: interpreter, "-x", script path, "arg1" = 4.
    assert(strstr(buf, "argc=4") != NULL);
    unlink(script); unlink(out);
    printf("[shebang] PASS\n");
    return 0;
}
