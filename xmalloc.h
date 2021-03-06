#ifndef XMALLOC_H
#define XMALLOC_H

#include <stddef.h>

void* xmalloc(size_t bytes);
void  xfree(void* ptr);
void* xrealloc(void* prev, size_t bytes);

void dump_flist();
void dump_buckets();

#endif
