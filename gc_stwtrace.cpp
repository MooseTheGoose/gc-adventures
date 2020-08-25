#include "gc_stwtrace.hpp"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#define DEBUG_ASSERT(x) assert(x)

struct gc_tree
{
  gc_tree *parent;
  gc_tree *children;
  gc_tree *next;
  void *data;
};

static const size_t heap_sz = 1 << 30; 
static align_t test_heap[heap_sz / sizeof(align_t)];
extern gclen_t strong_table[];

gclen_t strong_table[] = 
{
  /* No children */
  0,

  /* gc_tree */
  4,
  0, sizeof(void *), 2 * sizeof(void *), 3 * sizeof(void *)
};

#define MIN_ALLOCS  0x0008
#define MAX_ALLOCS  0x4000
static int alloc_threshold;
static int nallocs;

void gc_init() 
{
  gc_meta *begin = (gc_meta *)test_heap;
  begin->rrcnt = 1;
  begin->srtptr = 0;
  begin->refarray = 0;
  begin->mark = 1;
  begin->len = sizeof(gc_meta);
  begin->next = 0;

  alloc_threshold = MIN_ALLOCS;
  nallocs = 0;
}

void *gc_create_ref(gclen_t len, gclen_t srtptr, int flags)
{
  gclen_t true_len = (len + sizeof(gc_meta) + sizeof(align_t) - 1) &
                     ~(sizeof(align_t) - 1);

  gc_meta *prev = (gc_meta *)test_heap;
  gc_meta *curr = (gc_meta *)prev->next;
  char *region = (char *)(prev + 1);

  if(nallocs >= alloc_threshold)
  {
    gc_trace();
    int local_nallocs = nallocs;
    int local_threshold = alloc_threshold;

    if(local_threshold >= local_nallocs / 2 && local_threshold * 2 <= MAX_ALLOCS)
    { local_threshold *= 2; }
    else if(local_threshold <= local_nallocs / 4 && local_threshold / 2 >= MIN_ALLOCS)
    { local_threshold /= 2; }
  }

  /* Find space in between two nodes. */
  while(curr)
  {
    if(region + true_len < (char *)curr)
    {
      gc_meta *retmeta = (gc_meta *)region;
      memset(retmeta + 1, 0, len);
      retmeta->rrcnt = flags & ROOT_FLAG ? 1 : 0;
      retmeta->srtptr = srtptr;
      retmeta->refarray = flags & REFARRAY_FLAG ? 1 : 0;
      retmeta->mark = 0;
      retmeta->len = true_len;
      retmeta->next = curr;
      prev->next = retmeta;

      nallocs++;
      return retmeta + 1;
    }

    prev = curr;
    region = (char *)prev + prev->len;
    curr = curr->next;
  }

  /* If no space to sandwich between, try to allocate at end. */
  if(!curr && region + true_len < (char *)test_heap + heap_sz)
  {
    gc_meta *retmeta = (gc_meta *)region;
    memset(retmeta + 1, 0, len);
    retmeta->rrcnt = flags & ROOT_FLAG ? 1 : 0;
    retmeta->srtptr = srtptr;
    retmeta->refarray = flags & REFARRAY_FLAG ? 1 : 0;
    retmeta->mark = 0;
    retmeta->len = true_len;
    retmeta->next = 0;
    prev->next = retmeta;

    nallocs++;
    return retmeta + 1;
  }

  return 0;
}

void gc_dec_rrcnt(void *alloc)
{
  gc_meta *retmeta = ((gc_meta *)alloc) - 1;
  if(alloc) { retmeta->rrcnt--; } 
}

void gc_inc_rrcnt(void *alloc)
{
  gc_meta *retmeta = ((gc_meta *)alloc) - 1;
  if(alloc) { retmeta->rrcnt++; }
}

void gc_trace()
{
  gc_meta *prev_start = (gc_meta *)test_heap;
  gc_meta *start = prev_start->next;
  gc_meta *curr, *prev;

  curr = start;
  while(curr)
  { 
    curr->mark = 0; 
    curr = curr->next; 
  }

  curr = start;
  while(curr)
  {
    if(curr->rrcnt > 0 && !curr->mark)
    {
      gc_meta *curr_trace = curr;
      curr_trace->trace_next = 0;
      curr_trace->mark = 1;      

      while(curr_trace)
      {
        if(curr_trace->refarray)
        {
          void **children = (void **)(curr_trace + 1);
          gclen_t nchildren = curr_trace->srtptr;

          for(gclen_t i = 0; i < nchildren; i++)
          {
            gc_meta *check_mark = (gc_meta *)children[i];
            if(check_mark-- && !check_mark->mark)
            {
              check_mark->trace_next = curr_trace->trace_next;
              curr_trace->trace_next = check_mark;
              check_mark->mark = 1;
            }
          }
        }
        else
        {
          gclen_t nchildren = strong_table[curr_trace->srtptr];
          char *base = (char *)(curr_trace + 1);
          for(gclen_t i = curr_trace->srtptr + 1; nchildren--; i++)
          {
            gc_meta *check_mark = *(gc_meta **)(base + strong_table[i]);
            if(check_mark-- && !check_mark->mark)
            {
              check_mark->trace_next = curr_trace->trace_next;
              curr_trace->trace_next = check_mark;
              check_mark->mark = 1;
            }
          }
        }

        curr_trace = curr_trace->trace_next;
      }
    }
    curr = curr->next;
  }

  prev = prev_start;
  curr = start;
  int local_nallocs = nallocs;
  while(curr)
  {
    if(!curr->mark) 
    { 
      prev->next = curr->next;
      local_nallocs--; 
    }
    else { prev = curr; }
    curr = curr->next;
  }
  nallocs = local_nallocs;

//  DEBUG_ASSERT(!prev_start->next);
}

int main() 
{
  gc_init();

  while(1)
  {
    gc_tree *root = (gc_tree *)gc_create_ref(sizeof (gc_tree), 1, ROOT_FLAG);
    root->children = (gc_tree *)gc_create_ref(sizeof (gc_tree), 1, 0);
    root->children->parent = root;
    root->children->next = (gc_tree *)gc_create_ref(sizeof (gc_tree), 1, 0);
    root->children->next->parent = root;
    gc_dec_rrcnt(root);
    printf("%p\r\n", (void *)root);
  } 
  return 0; 
}