#include "gc_concur.hpp"

#include <vector>
#include <string.h>
#include <stdio.h>
#include <windows.h>
#include <assert.h>

using std::vector;

struct gc_ll
{ 
  gc_ll *prev, *next; 
  void *data;
};

struct gc_tree
{
  gc_tree *parent;
  gc_tree *children;
  gc_tree *prev, *next;
  void *data;
};

struct gc_refarray
{
  gclen_t len;
  /* Data */
};

/*
 *  Records of:
 *
 *  1: # of strong reference children (n)
 *  2-n+1: Children offsets
 */

static gcofs_t strong_table[] = 
{
  /* No child strong references*/
  0,

  /* gc_ll */
  3,
  0, sizeof(void *), 2 * sizeof(void *),

  /* gc_tree */
  5,
  0, sizeof(void *), 2 * sizeof(void *), 3 * sizeof(void *), 4 * sizeof(void *)
};

volatile static int invalidate_collector;
static gc_meta *volatile begin;
static const size_t heap_sz = 1 << 30;
static align_t *test_heap;

void gc_init()
{
  test_heap = (align_t *)malloc(heap_sz);
  CreateThread(0, 0, (LPTHREAD_START_ROUTINE)collector_thread, 0, 0, 0);
}

gcref_t gc_create_ref(gclen_t len, gcofs_t srtptr, int flags)
{
  gclen_t true_len = (len + sizeof(gc_meta) + sizeof(align_t) - 1) &
                     ~(sizeof(align_t) - 1);

  gc_meta *trail, *local_begin = begin;
  char *test = 0;
  gclen_t last_len = 0;
  int collected = 0;

  while(local_begin && local_begin->collected) 
  { 
    local_begin = local_begin->next; 
  }
  begin = local_begin;
  trail = local_begin;

  if((void *)local_begin > (void *)test_heap && 
     (char *)test_heap + true_len < (char *)local_begin)
  {
    gc_meta *retmeta = (gc_meta *)test_heap;

    retmeta->len = true_len;
    retmeta->prev = 0;
    retmeta->next = local_begin;
    local_begin->prev = retmeta;
 
    retmeta->srtptr = srtptr;
    retmeta->mark = 0;
    retmeta->collected = 0;
    retmeta->rrcnt = flags & ROOT_FLAG ? 1 : 0;
    begin = retmeta;

    invalidate_collector = 1;
    memset(retmeta + 1, 0, len);

    return retmeta + 1;
  }

  while(trail)
  {
    last_len = trail->len;
    test = (char *)trail + last_len;
    gc_meta *local_next = trail->next;
    gc_meta *local_prev = trail->prev;  

    if(trail->collected)
    {
      if(local_prev) 
      { local_prev->next = local_next; }
      if(local_next)
      { local_next->prev = local_prev; }

      collected = 1;
    }
    else if(test + true_len < (char *)local_next)
    {
      gc_meta *retmeta = (gc_meta *)test;

      retmeta->len = true_len;
      retmeta->prev = trail;
      retmeta->next = local_next;

      retmeta->srtptr = srtptr;
      retmeta->mark = 0;
      retmeta->collected = 0;
      retmeta->rrcnt = flags & ROOT_FLAG ? 1 : 0;
 
      if(local_next) { local_next->prev = retmeta; }
      trail->next = retmeta;

      invalidate_collector = 1;
      memset(retmeta + 1, 0, len);

      return retmeta + 1;
    }

    trail = local_next;
  }

  if(!trail && test + true_len < (char *)test_heap + heap_sz)
  {
    if(test)
    {
      gc_meta *retmeta = (gc_meta *)test;
      retmeta->len = true_len;
      retmeta->next = 0;
      retmeta->prev = (gc_meta *)(test - last_len);
      
      retmeta->srtptr = srtptr;
      retmeta->mark = 0;
      retmeta->collected = 0;
      retmeta->rrcnt = flags & ROOT_FLAG ? 1 : 0;
      retmeta->prev->next = retmeta;

      invalidate_collector = 1;
      memset(retmeta + 1, 0, len);
     
      return retmeta + 1;    
    }
    else
    {
      local_begin = (gc_meta *)test_heap;
      local_begin->len = true_len;
      local_begin->next = 0;
      local_begin->prev = 0;

      local_begin->srtptr = srtptr;
      local_begin->mark = 0;
      local_begin->collected = 0;
      local_begin->rrcnt = flags & ROOT_FLAG ? 1 : 0;
      begin = local_begin;

      invalidate_collector = 1;
      memset(local_begin + 1, 0, len);

      return local_begin + 1;
    }
  }


  if(collected) 
  { 
    return gc_create_ref(len, srtptr, flags); 
  }

  assert(0);

  return 0;
}

void collector_thread()
{
  gc_meta *local_meta;
  while(1)
  {
    /* Remove all marks. */
    while(invalidate_collector)
    {
      invalidate_collector = 0;
      local_meta = begin;
      while(local_meta && !invalidate_collector) 
      { 
        if(!local_meta->collected && !invalidate_collector)
        {
          local_meta->mark = 0;
          local_meta = local_meta->next;
        }
      }
    } 

    /* Move on to mark phase. */
    local_meta = begin;
    while(local_meta && !invalidate_collector)
    {
      if(!local_meta->collected && !invalidate_collector)
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
      }

      local_meta = local_meta->next;
    }

    /* 
     * Move on to the sweep phase only if mark phase was actually complete.
     * Note that stopping the sweep phase early is okay, since
     * unreachable objects will always stay unreachable. 
     */
    if(!invalidate_collector)
    {
      local_meta = begin;
      while(local_meta && !invalidate_collector)
      {
        if(!local_meta->collected && !invalidate_collector)
        {
          if(!local_meta->mark) { local_meta->collected = 1; }
          else { local_meta->mark = 0; }
          local_meta = local_meta->next;
        }
      }
    }
/* -------------------------------------------------- */
  }
}

void gc_dec_ref(void *alloc)
{
  gc_meta *metadata = (gc_meta *)alloc - 1;
//  printf("%p %lld %lld %lld\r\n", metadata, metadata->collected, metadata->rrcnt, metadata->mark);
  assert(!metadata->collected);
  metadata->rrcnt--; 
}

void gc_inc_ref(void *alloc)
{
  ((gc_meta *)alloc)[-1].rrcnt++;
}

int main()
{
  gc_init();
  
  gc_ll *myref1;
  while(1) 
  { 
    myref1 = (gc_ll *)gc_create_ref(sizeof(gc_ll), 1, ROOT_FLAG); 
    myref1->next = (gc_ll *)gc_create_ref(sizeof(gc_ll), 1, ROOT_FLAG);
    myref1->prev = (gc_ll *)gc_create_ref(sizeof(gc_ll), 1, ROOT_FLAG);
    gc_dec_ref(myref1->next);
    gc_dec_ref(myref1->prev);
    gc_dec_ref(myref1);
  }

  ExitThread(0);

  return 0;
}


