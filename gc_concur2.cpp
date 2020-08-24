#include "gc_concur2.hpp"

#include <vector>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include <assert.h>

using std::vector;

static gcofs_t strong_table[] = {0};
static const size_t heap_sz = 1 << 30;
static align_t *test_heap;

static volatile int please_collect;

void gc_init()
{
  test_heap = (align_t *)malloc(heap_sz);

  /*
   * It's very important that the test heap be
   * initialized before the sweeper rolls in.
   * Make begin serve as a smart node that both
   * the sweeper and allocator can always rely on
   * being there and being valid
   */
  gc_meta *begin = (gc_meta *)test_heap;
  begin->rrcnt = 1;
  begin->mark = 1;
  begin->sweep = 0;
  begin->refarray = 0;
  begin->srtptr = 0;
  begin->len = sizeof(gc_meta);
  begin->alloc_next = 0;
  begin->mark_next = 0;

  CreateThread(0, 0, (LPTHREAD_START_ROUTINE)sweeper_thread, 0, 0, 0);
}

/*
 *  For gc_create_ref and sweeper_thread, note that they share
 *  the mark list. Writes to this list will be synchronized
 *  and if the allocator needs to add a node, it will keep
 *  a private addme list and will add everything in that
 *  list for the next time it gets permission to write.
 */
void *gc_create_ref(gclen_t len, gcofs_t srtptr, int flags)
{
  gclen_t true_len = (len + sizeof(gc_meta) + sizeof(align_t) - 1) &
                     ~(sizeof(align_t) - 1);

  gc_meta *begin = (gc_meta *)test_heap;
  gc_meta *trail, *prev_trail;
  gc_meta *check_mark;
  char *test = (char *)(begin + 1);

  /*
   *  Collect when the tracer asks you nicely.
   */
  if(please_collect)
  {
    begin->mark_next = 0;
    trail = begin->alloc_next;
    prev_trail = begin;

    while(trail)
    {
      if(!trail->sweep)
      {
        trail->mark_next = begin->mark_next;
        begin->mark_next = trail;
        prev_trail->alloc_next = trail;
        prev_trail = trail;
      }
      trail = trail->alloc_next;
    }
    prev_trail->alloc_next = 0;
    please_collect = 0;
  }

  /*
   *  Use begin to ensure prev_trail is never null
   */
  prev_trail = begin;
  trail = prev_trail->alloc_next;


  /*
   *  If there's space at the beginning of the alloc list,
   *  put it at the beginning and mark it for adding to
   *  the mark list. Note that for all additions, it's very, very
   *  important to complete all initialization before adding it
   *  to the addme or mark lists.
   */
  if(trail != begin + 1 && test + true_len < (char *)trail)
  {
    gc_meta *retmeta = (gc_meta *)test;
    retmeta->rrcnt = 1;
    retmeta->mark = 1;
    retmeta->sweep = 0;
    retmeta->srtptr = srtptr;
    retmeta->len = true_len;
    retmeta->alloc_next = trail;
    memset(retmeta + 1, 0, len);
    begin->alloc_next = retmeta;

    return retmeta + 1; 
  }

  /*
   *  Otherwise, try to find space in the list
   *  which is available.
   */
  while(trail)
  {
    test = (char *)trail + trail->len;
    gc_meta *local_next = trail->alloc_next;

    if(test + true_len < (char *)local_next)
    {
      gc_meta *retmeta = (gc_meta *)test;
      retmeta->rrcnt = 1;
      retmeta->mark = 1;
      retmeta->sweep = 0;
      retmeta->srtptr = srtptr;
      retmeta->len = true_len;
      retmeta->alloc_next = local_next;
      memset(retmeta + 1, 0, len);
      trail->alloc_next = retmeta;

      return retmeta + 1;
    }
    else
    {
      prev_trail = trail;
      trail = local_next;
    }
  }

  /* 
   *  If no node was found in the middle, but there's still space
   *  at the end, allocate the node at the end while marking it
   *  for addition into the mark list.
   */
  if(!trail && test + true_len < (char *)test_heap + heap_sz)
  {
    gc_meta *retmeta = (gc_meta *)test;
    retmeta->rrcnt = 1;
    retmeta->mark = 1;
    retmeta->sweep = 0;
    retmeta->srtptr = srtptr;
    retmeta->len = true_len;
    retmeta->alloc_next = 0;
    memset(retmeta + 1, 0, len);
    prev_trail->alloc_next = retmeta;

    return retmeta + 1;
  }

  return 0;
}

void sweeper_thread()
{
  gc_meta *local_meta;
  gc_meta *local_prev;
  gc_meta *begin = (gc_meta *)test_heap;

  while(1)
  {
/* ------------------------------------ */
    while(please_collect) {}

    /* Clear */
    local_meta = begin->mark_next;
    while(local_meta)
    {
      local_meta->mark = 0;
      local_meta = local_meta->mark_next;
    }

    /* Mark */
    local_meta = begin->mark_next;
    while(local_meta)
    {
      if(local_meta->rrcnt > 0 && !local_meta->mark)
      {
        local_meta->mark = 1;
        vector<gc_meta *> rem = vector<gc_meta *>();
        rem.push_back(local_meta);
        
        while(rem.size())
        {
          gc_meta *current = rem.back();
          void *base = current + 1;
          rem.pop_back();

          /* 
           * TO-DO: Mark behavior changes with reference array 
           */
          gcofs_t nchildren = strong_table[current->srtptr];
          for(gcofs_t i = current->srtptr + 1; nchildren--; i++)
          {
            gc_meta *child_meta = *(gc_meta **)((char *)base + strong_table[i]);
            if(child_meta-- && !child_meta->mark)
            {
              child_meta->mark = 1;
              rem.push_back(child_meta);
            }
          }
        }
      }
      local_meta = local_meta->mark_next;
    }

    /* Sweep */
    local_meta = begin->mark_next;
    while(local_meta)
    {
      if(!local_meta->mark)
      {
        local_meta->sweep = 1;
      }
      local_meta = local_meta->mark_next;
    }
    please_collect = 1;
/* ------------------------------------ */
  }
}

void gc_dec_rrcnt(void *alloc)
{
  gc_meta *metadata = (gc_meta *)alloc - 1;
  if(alloc) metadata->rrcnt--; 
}

void gc_inc_rrcnt(void *alloc)
{
  gc_meta *metadata = (gc_meta *)alloc - 1;
  metadata->rrcnt++; 
}

int main()
{
  srand(time(0));
  gc_init();
  char *tracker = 0;

  while(1)
  {
    char *someref = (char *)gc_create_ref(0x80, 0, 0);
    printf("%p\r\n", someref);
    gc_dec_rrcnt(someref);
  }
  return 0;
}