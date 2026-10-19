#ifndef COPY_OPTIONS_H
#define COPY_OPTIONS_H

#include "common.h"

typedef struct {
    bool recursive;
    bool preserve;
    bool no_clobber;
    bool verbose;
    bool verify;
    bool progress;
    bool resume;
    bool dry_run;
} Options;

#endif
