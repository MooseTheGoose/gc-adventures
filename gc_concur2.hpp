#ifndef GC_CONCUR2_HPP
#define GC_CONCUR2_HPP

#include <stdint.h>

typedef uint64_t gcrcnt_t;
typedef uint64_t gclen_t;
typedef uint64_t align_t;
typedef uint64_t gcofs_t;

struct gc_meta
{
  volatile gcrcnt_t rrcnt        : 50;
  volatile gcrcnt_t mark         : 1;
  volatile gcrcnt_t sweep        : 1;
  volatile gcrcnt_t refarray     : 1;
  gcofs_t srtptr;
  gclen_t len;
  gc_meta *alloc_next;
  gc_meta *volatile mark_next;
};

#define REFARRAY_FLAG 1
void gc_init();
void *gc_create_ref(gclen_t len, gcofs_t srtptr, int flags);
void gc_dec_rrcnt(void *alloc);
void gc_inc_rrcnt(void *alloc);
void sweeper_thread();

#endif