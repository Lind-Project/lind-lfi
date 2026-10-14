#include "sys/sys.h"

#include <unistd.h>

int
sys_truncate(struct LFILinuxThread *t, lfiptr pathp, off_t length)
{
    char host_path[FILENAME_MAX];
    char *path = pathcopyresolve(t, pathp, host_path, sizeof(host_path));
    if (!path)
        return -LINUX_EINVAL;
    free(path);
    return HOST_ERR(int, truncate(host_path, length));
}
