/*
 * Host directory listing for fatimg. It lives in its own file because
 * <dirent.h> and FatFs both define a type called DIR.
 */
#include <dirent.h>
#include <string.h>

#include "hostdir.h"

int host_list_dir(const char *path, host_dir_callback callback, void *context) {
    DIR *d = opendir(path);
    if (!d) {
        return -1;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        callback(e->d_name, context);
    }

    closedir(d);
    return 0;
}
