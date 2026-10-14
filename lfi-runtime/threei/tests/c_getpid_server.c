// App cage for g_execimg: answers "g" commands with its getpid(), exits on "q".
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int cmd = atoi(argv[1]), out = atoi(argv[2]);
    char c;
    while (read(cmd, &c, 1) == 1 && c == 'g') {
        int v = (int) getpid();
        if (write(out, &v, sizeof v) != sizeof v) return 1;
    }
    return 0;
}
