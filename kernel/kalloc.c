// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define NUM_FRAMES 64
#define MAX_SWAP_PAGES 32000

// frame table entry
struct frame{
  int in_use;
  struct proc* p;
  uint64 va; // virtual address
  uint64 pa; // physical address
  int ref;
};
// Global frame table to track physical pages
struct frame frametable[NUM_FRAMES];
// Lock to acess frame table
struct spinlock framelock;

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&framelock, "frametable");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// Allocates memory for the user process
// Implemented because kalloc fails if the memory is full
// Instead we find a victim page and evict it from memory
void* kalloc_user(uint64 va,struct proc* p){
  // Find a empty slot in the frame table
  int slot=-1;
  uint64 pa=0;
  for(int i=0;i<NUM_FRAMES;i++){
    if(!frametable[i].in_use){
      slot=i;
      break;
    }
  }

  // If we find a slot
  // Get the physical address of the page from the free list
  if(slot!=-1) {
    void *mem=kalloc(); //gets pa from free list 
    if(mem!=0){
      pa=(uint64)mem;
    } 
    else{
      slot=-1; 
    }
  }

  // Find the victim page
  // Swap out the vicim page
  // get_victim and swap_out are not implemented yet
  if(slot==-1){
    slot=get_victim();
    struct frame *f=&frametable[slot];
    // We have process in frametable so pass the frame table slot
    swap_out(f);
    pa=f->pa;
  }

  // Update the frame table slot
  frametable[slot].pa=pa;
  frametable[slot].va=PGROUNDDOWN(va);
  frametable[slot].p=p;
  frametable[slot].in_use=1;
  frametable[slot].ref=1;

  p->resident_pages++; // Increase resident pages
  release(&framelock);
  return (void*)pa;
}

// Clear frame table entry for given physical address
void kfree_user(uint64 pa){
  acquire(&framelock);
  for(int i=0;i<NUM_FRAMES;i++){
    if(frametable[i].in_use && frametable[i].pa==pa){
      frametable[i].in_use=0;
      if(frametable[i].p){
        frametable[i].p->resident_pages--;
      }
      break;
    }
  }
  release(&framelock);
  kfree((void*)pa);
}