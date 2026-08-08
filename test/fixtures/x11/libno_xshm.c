/* Force stock Xlib to avoid MIT-SHM (guest SysV SHM cannot attach in host X).
 * PutImage works under hl; SHM attach to XQuartz does not without host-shared shmids.
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <stdio.h>
#include <stdlib.h>

/* Prefer weak override via interposition when preloaded first. */
Bool
XShmQueryExtension(Display *dpy)
{
    (void)dpy;
    return False;
}

Bool
XShmQueryVersion(Display *dpy, int *major, int *minor, int *pixmaps)
{
    (void)dpy;
    if (major) *major = 0;
    if (minor) *minor = 0;
    if (pixmaps) *pixmaps = 0;
    return False;
}

__attribute__((constructor))
static void
hl_no_xshm_init(void)
{
    const char *v = getenv("HL_NO_XSHM_VERBOSE");
    if (v && v[0] == '1')
        fprintf(stderr, "hl: libno_xshm: MIT-SHM disabled for this process\n");
}
