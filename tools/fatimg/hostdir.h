#pragma once

typedef void (*host_dir_callback)(const char *name, void *context);

/* Call `callback` for each entry of a host directory except "." and "..". Returns -1 if it cannot be read. */
int host_list_dir(const char *path, host_dir_callback callback, void *context);
