/*
 * FatFs configuration for the host tool: the FS system module's configuration,
 * minus the per-volume locking, which needs the module's mutexes.
 */
#include "../../sysmodules/fs/source/fat/ffconf.h"

#undef  FF_FS_REENTRANT
#define FF_FS_REENTRANT 0

#undef  FF_FS_LOCK
#define FF_FS_LOCK 0
