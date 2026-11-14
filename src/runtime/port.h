/* Portability: the few Windows C-runtime calls the port uses, for Linux / SteamOS builds.
 * On Windows this only includes the headers the calls come from. Elsewhere: _findfirst/_findnext/_findclose (only
 * .name is used) over glob(3), _mkdir, _stricmp. */
#ifndef PORT_H
#define PORT_H
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define _stricmp strcasecmp
#define _mkdir(p) mkdir((p), 0755)
#define _fseeki64 fseeko /* off_t is 64-bit on x86_64 Linux */
#define _ftelli64 ftello
#define _fileno fileno
#define _chsize_s ftruncate
#define _stat64 stat     /* struct _stat64 / _stat64() */

struct _finddata_t { char name[260]; };
typedef struct { glob_t g; size_t i; } port_find;

static inline void port_find_name(struct _finddata_t *fd, const char *path) {
    const char *s = strrchr(path, '/');
    snprintf(fd->name, sizeof fd->name, "%s", s ? s + 1 : path);
}

/* -1 when nothing matches, as the Windows call */
static inline intptr_t _findfirst(const char *pat, struct _finddata_t *fd) {
    port_find *f = (port_find *)calloc(1, sizeof *f);
    if (!f) return -1;
    if (glob(pat, 0, 0, &f->g) != 0 || !f->g.gl_pathc) { globfree(&f->g); free(f); return -1; }
    port_find_name(fd, f->g.gl_pathv[0]);
    return (intptr_t)f;
}
static inline int _findnext(intptr_t h, struct _finddata_t *fd) {
    port_find *f = (port_find *)h;
    if (++f->i >= f->g.gl_pathc) return -1;
    port_find_name(fd, f->g.gl_pathv[f->i]);
    return 0;
}
static inline int _findclose(intptr_t h) {
    port_find *f = (port_find *)h;
    globfree(&f->g);
    free(f);
    return 0;
}
#endif
#endif
