/* FE_TRACE_FILES=<file>: every path the port opens is appended to that file, one "<what> <mode> <path>" line
 * each (the check that the port reads only from its data folder: tools/check_data_reads.py). fopen is
 * wrapped at link time (build.py: -Wl,--wrap=fopen, which also covers stb_image); directory scans and the
 * ffmpeg command line are reported by their callers through ftrace(). */
#include <stdio.h>
#include <stdlib.h>
#include "hw.h"

FILE *__real_fopen(const char *path, const char *mode);

static FILE *trace;
static int trace_state; /* 0 not looked at, 1 on, -1 off */

void ftrace(const char *what, const char *path) {
    if (!trace_state) {
        const char *p = getenv("FE_TRACE_FILES");
        trace = p && p[0] ? __real_fopen(p, "a") : 0;
        trace_state = trace ? 1 : -1;
    }
    if (trace_state < 0) return;
    fprintf(trace, "%s %s\n", what, path ? path : "(null)");
    fflush(trace);
}

FILE *__wrap_fopen(const char *path, const char *mode) {
    FILE *f = __real_fopen(path, mode);
    if (trace_state >= 0) {
        char what[32];
        snprintf(what, sizeof what, "fopen-%s%s", mode, f ? "" : "-missing");
        ftrace(what, path);
    }
    return f;
}
