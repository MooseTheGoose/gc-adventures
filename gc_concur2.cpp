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
static gc_meta *volatile safe_next;

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
  begin->sweep  = 0;
  begin->refarray = 0;
  begin->addme = 0;
  begin->srtptr = 0;
  begin->len = sizeof(gc_meta);
  begin->alloc_next = 0;
  begin->mark_next = 0;
  begin->addme_next = 0;

  CreateThread(0, 0, (LPTHREAD_START_ROUTINE)sweeper_thread, 0, 0, 0);
}

/*
 *  For gc_create_ref and sweeper_thread, note that each have their
 *  own linked lists and that the only list they share is the addme
 *  list. The sweeper will remove from the addme list in a way such
 *  that the allocator never overwrite metadata in the addme list.
 */
void *gc_create_ref(gclen_t len, gcofs_t srtptr, int flags)
{
  gclen_t true_len = (len + sizeof(gc_meta) + sizeof(align_t) - 1) &
                     ~(sizeof(align_t) - 1);

  gc_meta *begin = (gc_meta *)test_heap;
  gc_meta *trail, *prev_trail;
  gc_meta *check_addme;
  char *test = (char *)(begin + 1);

  /*
   *  Use begin to ensure prev_trail is never null
   */
  prev_trail = begin;
  trail = prev_trail->alloc_next;

  /*
   *  Sweeper leaves begin->addme_next unmonitored,
   *  so if allocator collects prematurely, contents
   *  of begin->addme_next are undefined.
   */
  check_addme = begin->addme_next;
  if(check_addme && !check_addme->addme)
  { begin->addme_next = 0; }

  /*
   *  If there's space at the beginning of the alloc list,
   *  put it at the beginning and mark it for adding to
   *  the mark list. Note that for all additions, it's very, very
   *  important to complete all initialization before adding it
   *  to the addme list.
   */
  if(trail != begin + 1 && test + true_len < (char *)trail)
  {
    gc_meta *retmeta = (gc_meta *)test;
    retmeta->rrcnt = 1;
    retmeta->mark = 1;
    retmeta->sweep = 0;
    retmeta->addme = 1;
    retmeta->srtptr = srtptr;
    retmeta->len = true_len;
    retmeta->alloc_next = trail;
    retmeta->addme_next = begin->addme_next;
    memset(retmeta + 1, 0, len);
    begin->addme_next = retmeta;
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

    /*
     *  If trail is marked for sweeping, delete it,
     *  pin prev_trail, and adjust trail. Otherwise, 
     *  if there's space, allocate it and mark the space 
     *  for adding to mark list. Otherwise, move prev_trail 
     *  and trail up by one node.
     */
    if(trail->sweep)
    {
      prev_trail->alloc_next = local_next;
      trail = local_next;
    }
    else if(test + true_len < (char *)local_next)
    {
      gc_meta *retmeta = (gc_meta *)test;
      retmeta->rrcnt = 1;
      retmeta->mark = 1;
      retmeta->sweep = 0;
      retmeta->addme = 1;
      retmeta->srtptr = srtptr;
      retmeta->len = true_len;
      retmeta->alloc_next = local_next;
      retmeta->addme_next = begin->addme_next;
      memset(retmeta + 1, 0, len);
      begin->addme_next = retmeta;
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
    retmeta->addme = 1;
    retmeta->srtptr = srtptr;
    retmeta->len = true_len;
    retmeta->alloc_next = 0;
    retmeta->addme_next = begin->addme_next;
    memset(retmeta + 1, 0, len);
    begin->addme_next = retmeta;
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
    /*
     *  Every node in the addme list should have 
     *  an addme value of 1 when the sweeper messes
     *  around with a list and after it's done messing
     *  around, except for possibly begin->addme_next
     *  (which the allocator will personally monitor).
     *
     *  To do this, get the current head of the addme_list
     *  and until the nodes are null or the addme value is 0,
     *  add the node to the mark list and set its addme value to 0.
     *
     *  Afterwards, reiterate through the list (because it's volatile)
     *  and as soon as a node with addme value of 0 or null is encountered,
     *  remove it. Since allocator itself guarantees that every node it adds
     *  has an addme value of 1, the model of the list is sound.
     */
    local_meta = begin->addme_next;
    local_prev = begin;
    while(local_meta && local_meta->addme)
    {
      printf("%p\r\n", local_meta);

      local_meta->mark_next = begin->mark_next;
      begin->mark_next = local_meta;
      local_meta->addme = 0;
      local_meta = local_meta->addme_next;
    }
    if(local_prev != begin) 
    { 
      local_prev = begin->addme_next;
      local_meta = local_prev->addme_next;
      while(local_meta && local_meta->addme)
      {
        local_prev = local_meta;
        local_meta = local_meta->addme_next;
      }
      local_prev->addme_next = 0;
    }    

    local_meta = begin->mark_next;
    while(local_meta)
    {
      local_meta->mark = 0;
      local_meta = local_meta->mark_next;
    }

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

    local_meta = begin->mark_next;
    local_prev = begin;
    while(local_meta)
    {
      if(!local_meta->mark) 
      { 
        /* The order of operations is uber-important here. */
        local_prev->mark_next = local_meta->mark_next;
        local_meta->sweep = 1; 
      }
      else { local_prev = local_meta; }

      local_meta = local_meta->mark_next;
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

  while(1)
  {
    int size = rand() & 0xFF;
    if(size)
    {
      void *someref = gc_create_ref(rand() % 0x100 + 1, 0, 0);
      printf("%p\r\n", someref);
      gc_dec_rrcnt(someref);
    }
  }
  return 0;
}