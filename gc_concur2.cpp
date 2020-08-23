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

static gc_meta *volatile begin;
static volatile int begin_alloc_lock, begin_col_lock;
static const size_t heap_sz = 1 << 30;
static align_t *test_heap;

void gc_init()
{
  test_heap = (align_t *)malloc(heap_sz);
  CreateThread(0, 0, (LPTHREAD_START_ROUTINE)collector_thread, 0, 0, 0);
}

void *gc_create_ref(gclen_t len, gcofs_t srtptr, int flags)
{
  retry:

  gclen_t true_len = (len + sizeof(gc_meta) + sizeof(align_t) - 1) &
                     ~(sizeof(align_t) - 1);

  gc_meta *trail;
  char *test = 0;
  gclen_t last_len = 0;

  trail = begin;

  if((void *)trail > (void *)test_heap && 
     (char *)test_heap + true_len < (char *)trail)
  {
    gc_meta *retmeta = (gc_meta *)test_heap;

    /*
     *  It's very important for every initailization
     *  step to happen before formally linking the
     *  memory so as the mark and sweep algorithm
     *  doesn't die when this node is
     *  suddenly introduced.
     */
    retmeta->len = true_len;
    retmeta->srtptr = srtptr;
    retmeta->mark = 1;
    retmeta->rrcnt = 1;
    retmeta->col_lock = 0;
    retmeta->alloc_lock = 0;
    memset(retmeta + 1, 0, len);

    begin_col_lock = 1;
    if(!begin_alloc_lock)
    {
      retmeta->next = begin;
      begin = retmeta;
      begin_col_lock = 0;
      return retmeta + 1;
    }
    begin_col_lock = 0;
  }

  /*
   *  Note that trail may not be begin,
   *  but if collector retains links, 
   *  it is the head of a linked
   *  list which contains begin.
   */
  while(trail)
  {
    last_len = trail->len;
    test = (char *)trail + last_len;
    gc_meta *local_next = trail->next;

    if(test + true_len < (char *)local_next)
    {
      gc_meta *retmeta = (gc_meta *)test;
      retmeta->len = true_len;
      retmeta->next = local_next;
      retmeta->srtptr = srtptr;
      retmeta->mark = 1;
      retmeta->rrcnt = 1;
      retmeta->col_lock = 0;
      retmeta->alloc_lock = 0;
      memset(retmeta, 0, len);

      trail->col_lock = 1;
      if(!trail->alloc_lock)
      {
        trail->next = retmeta;
        trail->col_lock = 0;
        return retmeta + 1;
      }
      trail->col_lock = 0;
    }
    /* 
     * Similarly, local_next might not be the true next,
     * but local_next is the head of a linked list
     * which points to true next if collector retains links.
     */
    trail = local_next;
  }

  if(!trail && test + true_len < (char *)test_heap + heap_sz)
  {
    if(test)
    {
      gc_meta *retmeta = (gc_meta *)test;
      
      trail = (gc_meta *)(test - last_len);
      retmeta->len = true_len;
      retmeta->next = 0;
      retmeta->srtptr = srtptr;
      retmeta->mark = 1;
      retmeta->rrcnt = 1;
      memset(retmeta + 1, 0, len);

      trail->col_lock = 1;
      if(!trail->alloc_lock)
      {
        trail->next = retmeta;
        trail->col_lock = 0;
        return retmeta + 1;
      }
      trail->col_lock = 0;
    }
    else if(true_len < heap_sz)
    {
      trail = (gc_meta *)test_heap;
      trail->len = true_len;
      trail->next = 0;
      trail->srtptr = srtptr;
      trail->mark = 1;
      trail->rrcnt = 1;
      trail->alloc_lock = 0;
      trail->col_lock = 0;
      memset(trail + 1, 0, len);

      begin_col_lock = 1;
      if(!begin_alloc_lock)
      {
        begin = trail;
        begin_col_lock = 0;
        return trail + 1;
      }
      begin_col_lock = 0;
    }
  }

  goto retry;

  return 0;
}

void collector_thread()
{
  gc_meta *local_meta;

  while(1)
  {
/* ------------------------------------ */
    local_meta = begin;
    while(local_meta)
    { 
      local_meta->mark = 0; 
      local_meta = local_meta->next; 
    }

    local_meta = begin;
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
      local_meta = local_meta->next;
    }

    begin_alloc_lock = 1;
    if(!begin_col_lock)
    {
      gc_meta *local_begin = begin;
      while(local_begin && !local_begin->mark) 
      { local_begin = local_begin->next; }
      begin = local_begin; 
    }
    local_meta = begin;
    begin_alloc_lock = 0;

    while(local_meta)
    {
      if(!local_meta->mark)
      {
        local_meta->alloc_lock = 1;
        if(!local_meta->col_lock)
        {
          gc_meta *local_next = local_meta->next;
          if(local_next) { local_meta->next = local_next->next; }
        }
        local_meta->alloc_lock = 0;
      }
      local_meta = local_meta->next;
    }
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
  printf("%p\r\n", test_heap);
  getchar();
  void *familiar;

  while(1)
  {
    void *someref = gc_create_ref(rand() & 0xFF, 0, 0);
    if(someref != familiar)
    {
      familiar = someref;
      printf("%p\r\n", familiar);
    }
    gc_dec_rrcnt(someref);
  }
  return 0;
}