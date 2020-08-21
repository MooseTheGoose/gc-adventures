#include "gc_concur.hpp"

#include <vector>
#include <string.h>
#include <stdio.h>
#include <windows.h>

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
static const size_t heap_sz = 1 << 20;
static align_t test_heap[heap_sz / sizeof(align_t)];

void gc_init()
{
  CreateThread(0, 0, (LPTHREAD_START_ROUTINE)collector_thread, 0, 0, 0);
}

gcref_t gc_create_ref(gclen_t len, gcofs_t srtptr, int flags)
{
  gclen_t true_len = (len + sizeof(gc_meta) + sizeof(align_t) - 1) &
                     ~(sizeof(align_t) - 1);

  gc_meta *trail, *local_begin = begin;
  char *test = 0;
  gclen_t last_len = 0;

  while(local_begin && local_begin->collected) 
  { local_begin = local_begin->next; }
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
    memset(retmeta + 1, 0, len);

    begin = retmeta;
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

      trail = local_next;
      invalidate_collector = 1;
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
      memset(retmeta + 1, 0, len);
 
      if(local_next) { local_next->prev = retmeta; }
      trail->next = retmeta;

      return retmeta + 1;
    }
    else { trail = local_next; }
  }

  if(!trail && test + true_len < (char *)test_heap + heap_sz)
  {
    if(test)
    {
      gc_meta *retmeta = (gc_meta *)test;
      retmeta->len = true_len;
      retmeta->next = 0;
      retmeta->prev = (gc_meta *)(test - last_len);
      retmeta->prev->next = retmeta;
      
      retmeta->srtptr = srtptr;
      retmeta->mark = 0;
      retmeta->collected = 0;
      retmeta->rrcnt = flags & ROOT_FLAG ? 1 : 0;
      memset(retmeta + 1, 0, len);
     
      return retmeta + 1;    
    }
    else
    {
      begin = (gc_meta *volatile)test_heap;
      begin->len = true_len;
      begin->next = 0;
      begin->prev = 0;

      begin->srtptr = srtptr;
      begin->mark = 0;
      begin->collected = 0;
      begin->rrcnt = flags & ROOT_FLAG ? 1 : 0;
      memset(begin + 1, 0, len);

      return begin + 1;
    }
  }

  return 0;
}

void collector_thread()
{
  gc_meta *local_meta;
  while(1)
  {
/* -------------------------------------------------- */
    /* Remove all marks. */
    while(invalidate_collector)
    {
      invalidate_collector = 0;
      local_meta = begin;
      while(local_meta && !local_meta->collected && !invalidate_collector) 
      { 
        local_meta->mark = 0;
        local_meta = local_meta->next; 
      }
    } 

    printf("\r\n-----------------------------\r\n");
    printf("GC Report References:\r\n");

    /* Move on to mark phase. */
    local_meta = begin;
    while(local_meta && !invalidate_collector)
    {
      printf("%p %lld %lld %lld\r\n", local_meta, local_meta->collected, local_meta->mark, local_meta->rrcnt);
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
      printf("\r\nGC Report Sweeps:\r\n");

      local_meta = begin;
      while(local_meta && !local_meta->collected && !invalidate_collector)
      {
        if(!local_meta->mark) { printf("%p\r\n", local_meta); local_meta->collected = 1; }
        else { local_meta->mark = 0; }
        local_meta = local_meta->next;
      }
    } else { printf("Collector report invalidated\r\n"); }
    printf("-----------------------------\r\n");
    getchar();
/* -------------------------------------------------- */
  }
}

void gc_dec_ref(void *alloc)
{
  ((gc_meta *)alloc)[-1].rrcnt--;
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
    myref1->prev = (gc_ll *)gc_create_ref(sizeof(gc_ll), 1, ROOT_FLAG);
    myref1->next = (gc_ll *)gc_create_ref(sizeof(gc_ll), 1, ROOT_FLAG);
    Sleep(1000); 
    gc_dec_ref(myref1->prev);
    gc_dec_ref(myref1->next);
  }

  ExitThread(0);

  return 0;
}


