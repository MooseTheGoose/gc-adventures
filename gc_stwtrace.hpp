#ifndef GC_STWTRACE_HPP
#define GC_STWTRACE_HPP

/*
 *  Simple stop-the-world tracer for
 *  single threaded programs which uses
 *  internal variables to (fingers crossed)
 *  amortize the tracing and allocations.
 */

#include <stdint.h>

typedef int64_t gcrcnt_t;
typedef uint64_t gclen_t;
typedef uint64_t align_t;

struct gc_meta
{
  gcrcnt_t rrcnt;
  gclen_t srtptr : 60;
  gclen_t refarray : 1;
  gclen_t mark     : 1;
  gclen_t len;
  gc_meta *next;
  gc_meta *trace_next;
};

#define ROOT_FLAG     1
#define REFARRAY_FLAG 2

void gc_init();
void *gc_create_ref(gclen_t len, gclen_t srtptr, int flags);
void gc_dec_rrcnt(void *alloc);
void gc_inc_rrcnt(void *alloc);
void gc_trace();



#endif