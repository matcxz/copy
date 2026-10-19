#ifndef COPY_TREE_H
#define COPY_TREE_H

#include "options.h"
#include "progress.h"

int copy_entry(const char *source, const char *destination,
               const Options *options, Progress *progress);

#endif
