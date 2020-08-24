#ifndef GC_CONCUR_HPP
#define GC_CONCUR_HPP

#include <stdint.h>

typedef uint64_t gcrcnt_t;
typedef uint64_t gclen_t;
typedef uint64_t align_t;
typedef uint64_t gcofs_t;
typedef void *gcref_t;

struct gc_meta 
{
  volatile gcrcnt_t rrcnt : 60;     /* Roots strong reference count */
  volatile gcrcnt_t mark  : 1;
  volatile gcrcnt_t collected : 1;
  gcofs_t  srtptr;                  /* Strong reference table pointer */
  gclen_t  len;
  gc_meta *prev;
  gc_meta *next;
};

#define ROOT_FLAG 1
void gc_init();
gcref_t gc_create_ref(gclen_t len, gcofs_t srtptr, int flags);
void gc_dec_ref(gcref_t alloc);
void gc_inc_ref(gcref_t alloc);
void collector_thread();


#endif