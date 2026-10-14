#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    mkdir("/tmp/rd.d", 0755); mkdir("/tmp/rd.d/subdir", 0755);
    FILE *f = fopen("/tmp/rd.d/a_file_with_a_longer_name.txt", "w"); fclose(f);
    DIR *d = opendir("/tmp/rd.d");
    int dot = 0, sub = 0, file = 0, other = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") && e->d_type == DT_DIR) dot++;
        else if (!strcmp(e->d_name, "subdir") && e->d_type == DT_DIR) sub++;
        else if (!strcmp(e->d_name, "a_file_with_a_longer_name.txt") && e->d_type == DT_REG) file++;
        else if (strcmp(e->d_name, "..")) other++;
    }
    closedir(d);
    unlink("/tmp/rd.d/a_file_with_a_longer_name.txt"); rmdir("/tmp/rd.d/subdir"); rmdir("/tmp/rd.d");
    printf("dot=%d sub=%d file=%d other=%d\n", dot, sub, file, other);
    return !(dot == 1 && sub == 1 && file == 1 && other == 0);
}
